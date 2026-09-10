#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The two Q60 switchovers actually taking effect, on a real 60-member quorum.

The existing pair covers the wiring and the key generation and stops there:
feature_llmq_q60_regtest.py pins the 120-block formation lead, and
feature_llmq_q60_dkg.py completes one real llmq_defcon DKG. Neither asserts
the thing the switchover exists for -- that at nChainLocksV2ActivationHeight
ChainLock signing MOVES onto llmq_defcon, and that at
nInstantSendV2ActivationHeight InstantSend STARTS signing on it. One line
each resolves both (llmq::GetChainLocksLLMQType and GetInstantSendLLMQType,
src/llmq/options.cpp:123-139), every signer and every verifier funnels
through them, and a regression there is silent until a live network reaches
the height.

Both flips are asserted as a CHANGE OF PROFILE, observed on both sides of the
height, not as an absence:

  ChainLock   below: getbestchainlock().llmqType == llmq_test  (regtest's
              llmqTypeChainLocks, src/chainparams.cpp:1065)
              at/above:                          == llmq_defcon

  InstantSend below: no recovered signature exists under llmq_defcon for the
              transaction's InstantSend request id
              at/above: one does, and the transaction is locked

An earlier draft asserted absence instead -- "no ChainLock can form below the
height" -- and it was wrong, which is worth recording because it is the whole
reason this file is shaped the way it is: DashTestFramework.mine_quorum forms
EVERY enabled type in the same windows, so llmq_test, llmq_test_v17,
llmq_test_dip0024 and llmq_test_platform quorums all exist alongside the Q60
one, and ChainLocks below the switchover work perfectly well on llmq_test.
Absence would have passed only by accident.

The two heights are deliberately DIFFERENT (one DKG interval apart), because
equal heights cannot tell a resolver reading its own height from one reading
the other's.

Not in test_runner.py, for the same reason as feature_llmq_q60_dkg.py: it
raises the framework's node ceiling, and a parallel job with a different
ceiling gets a different port stride and can collide.
"""

import os

# Read at import time by test_framework, so it has to be set before the import.
os.environ.setdefault("TEST_RUNNER_MAX_NODES", "160")

from test_framework.messages import CTransaction, from_hex, hash256, ser_compact_size, ser_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal, force_finish_mnsync

# Consensus::LLMQType, src/llmq/params.h:1-29
Q60_TYPE = 7
Q60_SIZE = 60
Q60_MIN_SIZE = 44
Q60_THRESHOLD = 41
Q60_DKG_INTERVAL = 24
Q60_SIGNING_ACTIVE = 4

MN_COUNT = 65   # > Q60_SIZE, so membership is a real selection
MINE_CHUNK = 10  # blocks per mocktime bump, the ratio the framework's setup uses

FORMATION_LEAD = (Q60_SIGNING_ACTIVE + 1) * Q60_DKG_INTERVAL  # 120
CL_ACTIVATION = 600                                # a multiple of the DKG interval
IS_ACTIVATION = CL_ACTIVATION + Q60_DKG_INTERVAL   # 624, one interval later
FIRST_ENABLED_TIP = CL_ACTIVATION - FORMATION_LEAD - 1  # 479

# Three rounds, not one. Plain mining does not complete a DKG -- only
# mine_quorum's phase pacing does -- so the quorums this test signs with are
# exactly the ones formed here, and they have to still be signing-active at
# IS_ACTIVATION, which is 101 blocks above the first of them.
QUORUM_ROUNDS = 3


class LLMQQ60SwitchoverTest(DashTestFramework):
    def set_test_params(self):
        args = ["-testactivationheight=chainlocksv2@%d" % CL_ACTIVATION,
                "-testactivationheight=instantsendv2@%d" % IS_ACTIVATION]
        self.set_dash_test_params(MN_COUNT + 1, MN_COUNT, extra_args=[args] * (MN_COUNT + 1))
        # set AFTER set_dash_test_params, which resets these to the llmq_test values
        self.llmq_size = Q60_SIZE
        self.llmq_threshold = Q60_THRESHOLD

    # ---- helpers -------------------------------------------------------

    def mine_to(self, node, target):
        """Mine in chunks, bumping mocktime between them.

        One generate() of a long span never advances mocktime while it runs and
        the miner's block times creep past MAX_FUTURE_BLOCK_TIME; 369 blocks in
        one call ran 7019s ahead and the node rejected its own block.
        """
        while node.getblockcount() < target:
            step = min(MINE_CHUNK, target - node.getblockcount())
            self.bump_mocktime(1)
            self.generate(node, step)
        assert_equal(node.getblockcount(), target)

    def wait_for_chainlock(self, node, block_hash, timeout=90):
        """Wait on the block's own chainlock flag, bumping mocktime as we poll.

        The handler retries signing from a 5s scheduler task
        (src/llmq/chainlocks.cpp:60-65), and under mocktime that clock only
        moves when the test moves it.
        """
        def locked():
            self.bump_mocktime(1)
            try:
                block = node.getblock(block_hash)
                return block["confirmations"] > 0 and block["chainlock"]
            except Exception:
                return False
        self.wait_until(locked, timeout=timeout, sleep=1)

    def islock_request_id(self, node, txid):
        """The InstantSend request id, built exactly as the node builds it.

        Same construction as rpc_verifyislock.py, which is the only other test
        that asks the signing layer directly rather than reading a lock flag:
        ser_string("islock") || compact_size(len(vin)) || each prevout, double
        SHA256, reversed.
        """
        tx = from_hex(CTransaction(), node.getrawtransaction(txid))
        buf = ser_string(b"islock") + ser_compact_size(len(tx.vin))
        for txin in tx.vin:
            buf += txin.prevout.serialize()
        return hash256(buf)[::-1].hex()

    def q60_signed(self, node, txid, timeout=0):
        """Did the Q60 quorum produce a recovered signature for this txid?

        This is the discriminating question. A lock flag says a lock exists; it
        does not say which profile signed it, and below the switchover the old
        profile can produce one just as well.
        """
        request_id = self.islock_request_id(node, txid)

        def signed():
            self.bump_mocktime(1)
            try:
                return node.quorum("hasrecsig", Q60_TYPE, request_id, txid)
            except Exception:
                return False
        if timeout == 0:
            return signed()
        return self.wait_until(signed, timeout=timeout, sleep=1, do_assert=False)

    def wait_for_internal_islock(self, node, txid, timeout=60):
        """Always instantlock_internal, never instantlock.

        instantlock is also true for a transaction in a ChainLocked block, and
        this test deliberately runs with ChainLocks working -- so the plain
        field would report a lock that no ISLOCK ever produced.
        """
        def locked():
            self.bump_mocktime(1)
            try:
                return node.getrawtransaction(txid, True)["instantlock_internal"]
            except Exception:
                return False
        self.wait_until(locked, timeout=timeout, sleep=1)

    # ---- the run -------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        # SPORK_17 defaults to OFF, and without it CDKGSessionManager::UpdatedBlockTip
        # returns before any session handler: the phase thread sits in
        # WaitForNextPhase forever and it looks exactly like a quorum that will
        # not form. SPORK_2/3/19 are already on from the framework's setup.
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        assert_equal(len(node.masternodelist("status")), MN_COUNT)
        height = node.getblockcount()
        assert height < FIRST_ENABLED_TIP, (
            "setup left the chain at %d, at or past the formation lead %d -- raise CL_ACTIVATION"
            % (height, FIRST_ENABLED_TIP))

        self.log.info("Mine to the formation lead (%d) and form %d Q60 quorums",
                      FIRST_ENABLED_TIP, QUORUM_ROUNDS)
        self.mine_to(node, FIRST_ENABLED_TIP)
        assert "llmq_defcon" in node.quorum("list")

        for i in range(1, QUORUM_ROUNDS + 1):
            self.mine_quorum(llmq_type_name="llmq_defcon", llmq_type=Q60_TYPE,
                             expected_members=Q60_SIZE,
                             expected_contributions=Q60_SIZE,
                             expected_commitments=Q60_SIZE)
            self.log.info("  Q60 quorum %d formed, chain at height %d", i, node.getblockcount())

        entries = node.quorum("listextended")["llmq_defcon"]
        assert_greater_than_or_equal(len(entries), QUORUM_ROUNDS)
        newest = list(entries[0].items())[0][1]
        assert_greater_than_or_equal(newest["numValidMembers"], Q60_MIN_SIZE)

        # ---- ChainLock flip --------------------------------------------

        assert node.getblockcount() < CL_ACTIVATION
        self.log.info("BELOW %d: a ChainLock forms, and it is NOT on Q60", CL_ACTIVATION)
        below = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        self.wait_for_chainlock(node, below)
        best_below = node.getbestchainlock()
        self.log.info("  height=%d llmqType=%s", best_below["height"], best_below["llmqType"])
        assert best_below["height"] < CL_ACTIVATION
        assert_equal(best_below["llmqType"], "llmq_test")

        self.log.info("AT/ABOVE %d: the ChainLock must move onto Q60", CL_ACTIVATION)
        self.mine_to(node, CL_ACTIVATION)
        tip = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        self.wait_for_chainlock(node, tip)
        best_above = node.getbestchainlock()
        self.log.info("  height=%d llmqType=%s", best_above["height"], best_above["llmqType"])
        assert_greater_than_or_equal(best_above["height"], CL_ACTIVATION)
        assert_equal(best_above["llmqType"], "llmq_defcon")
        # The signed height is what the resolver keys on, and the verifier
        # resolves the same way: the node accepts its own lock back.
        assert node.verifychainlock(best_above["blockhash"], best_above["signature"],
                                    best_above["height"])

        # ---- InstantSend flip ------------------------------------------

        assert node.getblockcount() < IS_ACTIVATION
        self.log.info("BELOW %d: Q60 must NOT sign an InstantSend request", IS_ACTIVATION)
        early = node.sendtoaddress(node.getnewaddress(), 1)
        self.sync_mempools()
        assert not self.q60_signed(node, early, timeout=20), \
            "Q60 signed an InstantSend request below its activation height"

        self.log.info("AT/ABOVE %d: Q60 must sign, and the transaction must lock", IS_ACTIVATION)
        self.mine_to(node, IS_ACTIVATION)
        late = node.sendtoaddress(node.getnewaddress(), 1)
        self.sync_mempools()
        self.wait_for_internal_islock(node, late)
        assert self.q60_signed(node, late, timeout=30), \
            "the transaction locked, but not on Q60 -- the InstantSend resolver did not flip"

        islocks = node.getislocks([late])
        assert_equal(len(islocks), 1)
        assert_equal(islocks[0]["txid"], late)
        # A non-rotated profile's cycle hash sits on its own DKG interval.
        cycle_height = node.getblock(islocks[0]["cycleHash"])["height"]
        assert_equal(cycle_height % Q60_DKG_INTERVAL, 0)

        self.log.info("Both switchovers took effect at their own height, each observed on both sides")


if __name__ == "__main__":
    LLMQQ60SwitchoverTest().main()
