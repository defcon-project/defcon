#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_is_nonrotated_verify.py

A node that is not a masternode must accept an InstantSend lock signed by a
quorum older than one cycle.

On a non-rotated profile -- every InstantSend profile this chain runs -- the
signer picks the lowest-scoring of the `signingActiveQuorumCount` quorums
active at its own tip, and writes that quorum's base block into the lock as
`cycleHash`. Any of them may win, including one several cycles old.

The receiver used to re-derive the signer instead of reading it: it ran the
selection at `cycleHash + dkgInterval - 1`, where the candidates are that
quorum and the OLDER ones rather than that quorum and the newer ones. When an
older quorum outscored the real signer, the lock was thrown out as "invalid sig
in islock" -- measured on the devnet at roughly a fifth to three sevenths of
locks, by age. Nothing unsafe followed, because the masternodes hold the lock
and the double spend is refused; but the receiver lost the lock, waited out the
unlocked mining delay, and scored an honest sender as a bad peer.

The case is constructed here rather than waited for. Three quorums are mined,
the MIDDLE one signs -- chosen so that an older quorum exists to outscore it --
and the transaction is selected by computing the old re-derivation in Python
and keeping one it gets wrong. The lock is then delivered to the
non-masternode over P2P, which is the path that matters: a recovered signature
names its quorum outright and is not relayed to ordinary nodes, so the receiver
has nothing to go on but the lock.

On the unfixed binary this test fails where it waits for the lock.
'''

import os
import shutil
import struct
from hashlib import sha256

from test_framework.messages import (
    COutPoint,
    hash256,
    msg_isdlock,
    ser_compact_size,
    ser_string,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

LLMQ_TYPE_TEST_INSTANTSEND = 104
DKG_INTERVAL = 24


def internal(hex_hash):
    """A hash's bytes as the node serialises them: the printed hex, reversed."""
    return bytes.fromhex(hex_hash)[::-1]


def sha256d(data):
    return sha256(sha256(data).digest()).digest()


def score(llmq_type, quorum_hash_hex, selection_hash):
    """One candidate's score: SHA256d(uint8 type || quorumHash || selectionHash)."""
    return sha256d(bytes([llmq_type]) + internal(quorum_hash_hex) + selection_hash)


def lowest_scoring(llmq_type, quorum_hashes, selection_hash):
    """SelectQuorumForSigningAt on a non-rotated type: the lowest-scoring candidate."""
    return min(quorum_hashes, key=lambda q: score(llmq_type, q, selection_hash))


class LLMQInstantSendNonRotatedVerifyTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(5, 4, [["-llmqtestinstantsenddip0024=llmq_test_instantsend"]] * 5)
        self.set_dash_llmq_test_params(4, 3)

    def run_test(self):
        node = self.nodes[0]

        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        # 1 keeps InstantSend on but stops the masternodes signing mempool
        # transactions of their own accord. Without that they lock the
        # transaction before this test can deliver its own lock, and the test
        # would then pass on a lock it did not construct -- which is how it
        # failed the first time it was run.
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 1)
        node.sporkupdate("SPORK_3_INSTANTSEND_BLOCK_FILTERING", 0)
        self.wait_for_sporks_same()

        self.log.info("Mine three non-rotating InstantSend quorums")
        for _ in range(3):
            self.mine_quorum(llmq_type_name="llmq_test_instantsend", llmq_type=LLMQ_TYPE_TEST_INSTANTSEND)

        # `quorum list` on its own answers signingActiveQuorumCount of them --
        # two here -- and the point is to reach past that, to a quorum that is
        # still a legal signer but has an older one behind it.
        quorums = node.quorum("list", 10)["llmq_test_instantsend"]
        assert len(quorums) >= 3, f"expected at least three quorums, got {quorums}"
        # Newest first, as the RPC reports them.
        middle, oldest = quorums[1], quorums[2]
        # The fix only accepts a quorum that is still signing-active, so the
        # signer this test picks has to be one: that is what makes the lock a
        # legal one to have signed, and the old rejection a defect.
        active = node.quorum("list")["llmq_test_instantsend"]
        assert middle in active, f"the middle quorum is not signing-active any more: {active}"
        assert oldest not in active, "the older quorum is still active; it cannot play the outscorer"
        middle_height = node.getblock(middle)["height"]
        self.log.info(f"the signer is the middle quorum {middle[:16]} at height {middle_height}, "
                      f"with {oldest[:16]} older than it")

        # The old re-derivation differs from the truth only once the signing
        # quorum is more than one cycle behind the tip; below that it runs at
        # the tip and lands on the right quorum by accident.
        while node.getblockcount() <= middle_height + DKG_INTERVAL:
            self.generate(node, 1)

        self.log.info("Pick a transaction the old re-derivation gets wrong")
        txid, request_id, inputs = self.find_misleading_tx(middle, oldest)

        self.log.info("The middle quorum signs it, and only it")
        for mn in self.mninfo:
            mn.node.quorum("sign", LLMQ_TYPE_TEST_INSTANTSEND, request_id, txid, middle)
        self.wait_for_recovered_sig(request_id, txid, LLMQ_TYPE_TEST_INSTANTSEND, 15)
        rec_sig = self.mninfo[0].node.quorum("getrecsig", LLMQ_TYPE_TEST_INSTANTSEND, request_id, txid)
        assert_equal(rec_sig["quorumHash"], middle)

        # Put the receiver on the path this test is about, and prove it is there.
        #
        # A recovered signature names its quorum outright, and it does reach
        # ordinary nodes: holding one, the receiver skips the reconstruction
        # entirely and accepts the lock on either binary -- the test would then
        # prove nothing, which is what its first version did. Cutting the node
        # off with `isolate_node` is no good either, because that turns the
        # network off and refuses the very connection the lock is delivered
        # over; dropping the node-to-node links alone is no good because they
        # come back on their own.
        #
        # So the receiver is restarted without peers and without its recovered
        # signature database: the chain, the wallet and the mempool survive, the
        # shortcut does not. `-connect=0` keeps it alone with the one peer this
        # test attaches.
        self.log.info("Restart the receiver with no peers and no recovered signatures")
        recsigdb = os.path.join(node.datadir, self.chain, "llmq", "recsigdb")
        self.stop_node(0)
        assert os.path.isdir(recsigdb), f"no recovered-signature database at {recsigdb}"
        shutil.rmtree(recsigdb)
        self.start_node(0, extra_args=self.extra_args[0] + ["-connect=0"])

        assert_equal(node.quorum("hasrecsig", LLMQ_TYPE_TEST_INSTANTSEND, request_id, txid), False)
        assert_equal(node.getrawtransaction(txid, True)["instantlock_internal"], False)
        peer = node.add_p2p_connection(P2PInterface())
        assert_equal(node.getconnectioncount(), 1)

        self.log.info("Deliver the lock over P2P, and expect it to be accepted")
        islock = msg_isdlock(1, inputs, int(txid, 16), int(middle, 16), bytes.fromhex(rec_sig["sig"]))
        peer.send_message(islock)
        # The message has to be on the receiver's side of the socket before the
        # wait below means anything: a lock that never arrived and a lock that
        # was rejected look identical from the RPC, and the first version of
        # this test could not tell them apart.
        peer.sync_with_ping()
        received = self.nodes[0].getpeerinfo()[0]["bytesrecv_per_msg"]
        self.log.info(f"the receiver's byte counters: {received}")
        assert "isdlock" in received, f"the lock never reached the node: {received}"

        def locked():
            self.bump_mocktime(1)
            return node.getrawtransaction(txid, True)["instantlock_internal"]

        self.wait_until(locked, timeout=30, sleep=1)

        self.log.info("And the stored lock names the quorum that signed it")
        islocks = node.getislocks([txid])
        assert_equal(len(islocks), 1)
        assert_equal(islocks[0]["cycleHash"], middle)


    def find_misleading_tx(self, signer, older, attempts=8):
        """
        A transaction whose request id makes the OLD re-derivation prefer
        `older` to `signer` -- the case the receiver used to reject. Computed
        here rather than waited for: with two candidates it is about one
        transaction in two, and a test that hoped for it would be a coin flip.
        """
        node = self.nodes[0]
        for _ in range(attempts):
            txid = node.sendtoaddress(node.getnewaddress(), 1)
            self.sync_mempools()
            raw = node.getrawtransaction(txid, True)
            request_id_buf = ser_string(b"islock") + ser_compact_size(len(raw["vin"]))
            inputs = []
            for vin in raw["vin"]:
                point = COutPoint(int(vin["txid"], 16), int(vin["vout"]))
                request_id_buf += point.serialize()
                inputs.append(point)
            selection_hash = hash256(request_id_buf)
            if lowest_scoring(LLMQ_TYPE_TEST_INSTANTSEND, [signer, older], selection_hash) == older:
                return txid, selection_hash[::-1].hex(), inputs
            self.log.info(f"  {txid[:16]} would have resolved correctly; trying another")
        raise AssertionError("no transaction found whose re-derivation lands on the older quorum")


if __name__ == '__main__':
    LLMQInstantSendNonRotatedVerifyTest().main()
