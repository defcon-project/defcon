#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_is_named_quorum_window.py

A node that is not a masternode must accept an InstantSend lock signed by the
quorum that has just stopped being one of the `signingActiveQuorumCount`
newest -- in the blocks right after a new quorum's commitment is mined.

The signer chooses among the quorums that were signing-active
SIGN_HEIGHT_OFFSET blocks below its tip, so for a few blocks after a new
commitment it can still pick the oldest of the previous set. The receiver
takes the quorum the lock names only if it counts that quorum as active; with
`signingActiveQuorumCount` as the bound, the one that had just dropped out was
not, the lock fell back to replaying the selection, and the replay could land
on an older quorum and reject the lock as "invalid sig in islock". The bound
is now IsQuorumActive, the one the node already applies to sig shares and
recovered signatures: the `keepOldConnections` newest, which is more than
`signingActiveQuorumCount` on every profile (three against two here).

The bound has a far edge too, and the test fixes it: a lock naming the quorum
just beyond it -- the fourth newest, with `keepOldConnections` 3 -- is not
taken on the named path. Its replay runs instead, lands on an older quorum,
and the lock is rejected. A change that took every quorum the node holds would
pass the rest of this test and fail there.

Constructed, not waited for. Four quorums are mined and the third newest signs
a transaction while its sig shares are still taken; a fifth quorum is mined,
which puts that signer beyond the bound, and the new third newest signs
another. Each transaction is chosen so that its replay prefers the quorum
below the signer. The receiver is restarted with no peers and no recovered
signatures (see feature_llmq_is_nonrotated_verify.py for why both are needed)
and its tip is taken back to the block that mined the newest commitment, which
is where the window is; the locks are then delivered over P2P, the one from
beyond the bound first.

On the binary before the fix this test fails where it waits for the second
lock; on one that takes any quorum it holds, where it expects the first lock to
be rejected.
'''

import os
import shutil

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


def score(llmq_type, quorum_hash_hex, selection_hash):
    """One candidate's score: SHA256d(uint8 type || quorumHash || selectionHash)."""
    return hash256(bytes([llmq_type]) + internal(quorum_hash_hex) + selection_hash)


def lowest_scoring(llmq_type, quorum_hashes, selection_hash):
    """The quorum the selection picks: the lowest score, compared as bytes."""
    return min(quorum_hashes, key=lambda q: score(llmq_type, q, selection_hash))


class LLMQInstantSendNamedQuorumWindowTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(5, 4, [["-llmqtestinstantsenddip0024=llmq_test_instantsend"]] * 5)
        self.set_dash_llmq_test_params(4, 3)

    def run_test(self):
        node = self.nodes[0]

        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        # 1 keeps InstantSend on but stops the masternodes signing mempool
        # transactions of their own accord, so the only locks are the ones built here.
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 1)
        node.sporkupdate("SPORK_3_INSTANTSEND_BLOCK_FILTERING", 0)
        self.wait_for_sporks_same()

        # The locks' transactions have to survive the receiver being taken back
        # a few blocks below. Spending freshly matured coinbases, they would not:
        # the rewind makes some of them immature again and the transaction leaves
        # the mempool. So they spend ordinary outputs made here, a hundred blocks
        # deep by the time they are built.
        self.log.info("Make ordinary outputs for the transactions to spend")
        addresses = [node.getnewaddress() for _ in range(64)]
        funding = node.sendmany("", {a: 5 for a in addresses})
        self.generate(node, 1)
        def address_of(script):
            return script.get("address") or (script.get("addresses") or [None])[0]

        self.spendable = [{"txid": funding, "vout": o["n"]}
                          for o in node.getrawtransaction(funding, True)["vout"]
                          if o["value"] == 5 and address_of(o["scriptPubKey"]) in addresses]
        assert_equal(len(self.spendable), 64)

        self.log.info("Mine four non-rotating InstantSend quorums")
        for _ in range(4):
            self.mine_quorum(llmq_type_name="llmq_test_instantsend", llmq_type=LLMQ_TYPE_TEST_INSTANTSEND)

        # Newest first, as the RPC reports them. The masternodes take sig shares
        # from IsQuorumActive's set -- the three newest -- so the third newest
        # can still sign now; once a fifth quorum is mined it will be the fourth
        # newest, just beyond the receiver's bound. Its transaction stays off the
        # network until the receiver is ready, or the next quorum's blocks would
        # mine it.
        four = node.quorum("list", 10)["llmq_test_instantsend"]
        assert len(four) >= 4, f"expected at least four quorums, got {four}"
        beyond, below_beyond = four[2], four[3]
        self.log.info("Pick a transaction the replay gets wrong for the quorum that will be beyond the bound")
        beyond_txid, beyond_id, beyond_inputs, beyond_hex = self.find_misleading_tx(beyond, below_beyond, broadcast=False)
        self.log.info(f"The third newest quorum {beyond[:16]} signs it, and only it")
        beyond_sig = self.sign_with(beyond, beyond_id, beyond_txid)

        self.log.info("Mine the fifth quorum")
        self.mine_quorum(llmq_type_name="llmq_test_instantsend", llmq_type=LLMQ_TYPE_TEST_INSTANTSEND)
        quorums = node.quorum("list", 10)["llmq_test_instantsend"]
        assert len(quorums) >= 5, f"expected at least five quorums, got {quorums}"
        newest, signer, older = quorums[0], quorums[2], quorums[3]
        assert_equal(older, beyond)
        assert_equal(quorums[4], below_beyond)
        newest_mined = node.getblock(node.quorum("info", LLMQ_TYPE_TEST_INSTANTSEND, newest)["minedBlock"])["height"]
        signer_height = node.getblock(signer)["height"]
        beyond_height = node.getblock(beyond)["height"]
        self.log.info(f"newest quorum {newest[:16]} mined at {newest_mined}; the signer is {signer[:16]} "
                      f"(base {signer_height}), with {older[:16]} (base {beyond_height}) beyond the bound")

        self.log.info("Pick a transaction the old replay gets wrong")
        txid, request_id, inputs, _ = self.find_misleading_tx(signer, older)

        self.log.info("The third newest quorum signs it, and only it")
        rec_sig = self.sign_with(signer, request_id, txid)

        self.log.info("Restart the receiver with no peers and no recovered signatures")
        recsigdb = os.path.join(node.datadir, self.chain, "llmq", "recsigdb")
        self.stop_node(0)
        assert os.path.isdir(recsigdb), f"no recovered-signature database at {recsigdb}"
        shutil.rmtree(recsigdb)
        self.start_node(0, extra_args=self.extra_args[0] + ["-connect=0"])
        assert_equal(node.quorum("hasrecsig", LLMQ_TYPE_TEST_INSTANTSEND, request_id, txid), False)
        assert_equal(node.quorum("hasrecsig", LLMQ_TYPE_TEST_INSTANTSEND, beyond_id, beyond_txid), False)

        # mine_quorum pads SIGN_HEIGHT_OFFSET blocks past each commitment, which
        # carries the tip out of the window. Take the receiver back to the block
        # that mined the newest commitment: there the signer is no longer
        # signing-active, but was so SIGN_HEIGHT_OFFSET blocks below.
        self.log.info(f"Take the receiver's tip back to {newest_mined}, inside the window")
        node.invalidateblock(node.getblockhash(newest_mined + 1))
        assert_equal(node.getblockcount(), newest_mined)
        active = node.quorum("list")["llmq_test_instantsend"]
        assert signer not in active, f"the signer is still signing-active at the receiver: {active}"
        assert newest in active, f"the newest quorum is not active at the receiver: {active}"
        assert node.getblockcount() > signer_height + DKG_INTERVAL, "the lock would not reach the replay"
        assert_equal(node.getrawtransaction(txid, True)["instantlock_internal"], False)

        # The kept-back transaction joins the receiver's mempool only now, so no
        # block has taken it; with no peers it goes nowhere else.
        assert_equal(node.sendrawtransaction(beyond_hex), beyond_txid)
        assert_equal(node.getrawtransaction(beyond_txid, True)["instantlock_internal"], False)

        peer = node.add_p2p_connection(P2PInterface())
        assert_equal(node.getconnectioncount(), 1)

        def locked(which):
            self.bump_mocktime(1)
            return node.getrawtransaction(which, True)["instantlock_internal"]

        def rejected_twice():
            # Both passes of the verification: the replay at the signing height,
            # then the one a cycle earlier. Neither lands on the signer.
            with open(node.debug_log_path, encoding="utf-8", errors="replace") as log:
                lines = [l for l in log if f"txid={beyond_txid}" in l and "invalid sig in islock" in l]
            return len(lines) >= 2

        self.log.info("Deliver the lock from beyond the bound, and expect it to be rejected")
        self.deliver(peer, beyond_inputs, beyond_txid, beyond, beyond_sig)
        self.wait_until(lambda: rejected_twice() or locked(beyond_txid), timeout=30, sleep=1)
        assert not locked(beyond_txid), "a lock from beyond the bound was taken on the named path"
        assert rejected_twice(), "the lock from beyond the bound was neither rejected nor taken"
        # The second pass scores its sender as misbehaving, well short of a disconnect.
        assert_equal(node.getconnectioncount(), 1)

        self.log.info("Deliver the lock from inside the bound, and expect it to be accepted")
        self.deliver(peer, inputs, txid, signer, rec_sig)
        self.wait_until(lambda: locked(txid), timeout=30, sleep=1)

        self.log.info("And the stored lock names the quorum that signed it")
        islocks = node.getislocks([txid])
        assert_equal(len(islocks), 1)
        assert_equal(islocks[0]["cycleHash"], signer)

    def sign_with(self, quorum, request_id, txid):
        """Every masternode signs with the given quorum; the recovered signature comes back."""
        for mn in self.mninfo:
            mn.node.quorum("sign", LLMQ_TYPE_TEST_INSTANTSEND, request_id, txid, quorum)
        self.wait_for_recovered_sig(request_id, txid, LLMQ_TYPE_TEST_INSTANTSEND, 15)
        rec_sig = self.mninfo[0].node.quorum("getrecsig", LLMQ_TYPE_TEST_INSTANTSEND, request_id, txid)
        assert_equal(rec_sig["quorumHash"], quorum)
        return rec_sig

    def deliver(self, peer, inputs, txid, quorum, rec_sig):
        """One ISDLOCK over P2P, and proof that it arrived."""
        node = self.nodes[0]
        before = node.getpeerinfo()[0]["bytesrecv_per_msg"].get("isdlock", 0)
        islock = msg_isdlock(1, inputs, int(txid, 16), int(quorum, 16), bytes.fromhex(rec_sig["sig"]))
        peer.send_message(islock)
        peer.sync_with_ping()
        received = node.getpeerinfo()[0]["bytesrecv_per_msg"]
        assert received.get("isdlock", 0) > before, f"the lock never reached the node: {received}"

    def find_misleading_tx(self, signer, older, broadcast=True, attempts=32):
        """
        A transaction whose request id makes the replay prefer `older` to
        `signer`. As in feature_llmq_is_nonrotated_verify.py: thirty-two
        attempts fail one run in four billion by bad luck alone. Without
        `broadcast` the transaction is built and signed but sent nowhere.
        """
        node = self.nodes[0]
        for _ in range(attempts):
            utxo = self.spendable.pop()
            raw_hex = node.createrawtransaction([utxo], {node.getnewaddress(): 4.999})
            signed = node.signrawtransactionwithwallet(raw_hex)
            assert signed["complete"], signed
            if broadcast:
                txid = node.sendrawtransaction(signed["hex"])
                self.sync_mempools()
                raw = node.getrawtransaction(txid, True)
            else:
                raw = node.decoderawtransaction(signed["hex"])
                txid = raw["txid"]
            request_id_buf = ser_string(b"islock") + ser_compact_size(len(raw["vin"]))
            inputs = []
            for vin in raw["vin"]:
                point = COutPoint(int(vin["txid"], 16), int(vin["vout"]))
                request_id_buf += point.serialize()
                inputs.append(point)
            selection_hash = hash256(request_id_buf)
            if lowest_scoring(LLMQ_TYPE_TEST_INSTANTSEND, [signer, older], selection_hash) == older:
                return txid, selection_hash[::-1].hex(), inputs, signed["hex"]
            self.log.info(f"  {txid[:16]} would have resolved correctly; trying another")
        raise AssertionError("no transaction found whose replay lands on the older quorum")


if __name__ == '__main__':
    LLMQInstantSendNamedQuorumWindowTest().main()
