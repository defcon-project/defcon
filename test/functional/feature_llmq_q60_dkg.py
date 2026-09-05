#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""One real Q60 DKG round on regtest, with real BLS keys.

feature_llmq_q60_regtest.py covers the wiring: that regtest registers
llmq_defcon when given a switchover height, and opens formation exactly at the
lead. It never forms one, because it has no masternodes. This does.

The scale simulator answers what Q60 does across 150-15000 masternodes, but it
answers it by calling the selection functions on synthetic lists -- no BLS, no
DKG phases, no commitment. The one thing it cannot show is that 60/44/41 is a
profile the node can actually complete a distributed key generation on. That
needs more masternodes than the quorum is wide, which is why no existing test
does it: 65 daemons is the smallest network where selection is real (60 of 65
chosen, so membership churns) and the DKG is a genuine 60-member session.

What it pins: the session runs with exactly `size` members, the mined
commitment carries at least `minSize` valid ones, and the quorum reaches the
`quorum list` of a node that was never a member.
"""

import os

# Both of these need more daemons than the framework's historical 20-node
# ceiling, and MAX_NODES is read at import time -- so it has to be set before
# test_framework comes in. Set here rather than in the caller's environment so
# the test is runnable on its own. Not in test_runner.py's list: a job with a
# different ceiling gets a different port stride, and two of those in parallel
# can collide.
os.environ.setdefault("TEST_RUNNER_MAX_NODES", "160")

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal, force_finish_mnsync

# llmq_defcon, from Consensus::available_llmqs
Q60_TYPE = 7
Q60_SIZE = 60
Q60_MIN_SIZE = 44
Q60_DKG_INTERVAL = 24
Q60_SIGNING_ACTIVE = 4

MN_COUNT = 65  # > Q60_SIZE, so membership is a real selection and not everyone
ACTIVATION = 480  # a multiple of the DKG interval, above where setup leaves the chain
MINE_CHUNK = 10   # blocks per mocktime bump, the ratio the framework's own setup uses
FORMATION_LEAD = (Q60_SIGNING_ACTIVE + 1) * Q60_DKG_INTERVAL  # 120
FIRST_ENABLED_TIP = ACTIVATION - FORMATION_LEAD - 1  # 1319


class LLMQQ60DKGTest(DashTestFramework):
    def set_test_params(self):
        args = ["-testactivationheight=chainlocksv2@%d" % ACTIVATION]
        self.set_dash_test_params(MN_COUNT + 1, MN_COUNT, extra_args=[args] * (MN_COUNT + 1))
        # set AFTER set_dash_test_params, which resets these to the llmq_test values
        self.llmq_size = Q60_SIZE
        self.llmq_threshold = 41

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        # SPORK_17_QUORUM_DKG_ENABLED defaults to OFF, and without it
        # CDKGSessionManager::UpdatedBlockTip returns before it reaches any
        # session handler -- the phase thread sits in WaitForNextPhase forever
        # and the failure looks exactly like a quorum that will not form.
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("%d masternodes are ENABLED", MN_COUNT)
        assert_equal(len(node.masternodelist("status")), MN_COUNT)

        self.log.info("Below the formation lead the profile is not enabled")
        assert "llmq_defcon" not in node.quorum("list")

        height = node.getblockcount()
        assert height < FIRST_ENABLED_TIP, (
            "setup left the chain at %d, at or past the formation lead %d -- raise ACTIVATION"
            % (height, FIRST_ENABLED_TIP))

        # In chunks, bumping mocktime between them. One generate() of the whole
        # span never advances mocktime while it runs, and the miner's block times
        # creep past MAX_FUTURE_BLOCK_TIME: 369 blocks in one call ran 7019s ahead
        # and the node rejected its own block as time-too-new.
        self.log.info("Mining %d blocks to the formation lead", FIRST_ENABLED_TIP - height)
        while node.getblockcount() < FIRST_ENABLED_TIP:
            step = min(MINE_CHUNK, FIRST_ENABLED_TIP - node.getblockcount())
            self.bump_mocktime(1)
            self.generate(node, step)
        assert_equal(node.getblockcount(), FIRST_ENABLED_TIP)
        assert "llmq_defcon" in node.quorum("list")

        self.log.info("Mining one real %d-member DKG round", Q60_SIZE)
        self.mine_quorum(llmq_type_name="llmq_defcon", llmq_type=Q60_TYPE,
                         expected_members=Q60_SIZE,
                         expected_contributions=Q60_SIZE,
                         expected_commitments=Q60_SIZE)

        entries = node.quorum("listextended")["llmq_defcon"]
        assert_greater_than_or_equal(len(entries), 1)
        quorum_hash, info = list(entries[0].items())[0]
        self.log.info("Q60 quorum %s: numValidMembers=%d healthRatio=%s",
                      quorum_hash, info["numValidMembers"], info["healthRatio"])

        self.log.info("The mined commitment carries at least minSize valid members")
        assert_greater_than_or_equal(info["numValidMembers"], Q60_MIN_SIZE)

        self.log.info("The session really had %d members, drawn from %d masternodes",
                      Q60_SIZE, MN_COUNT)
        members = node.quorum("info", Q60_TYPE, quorum_hash)["members"]
        assert_equal(len(members), Q60_SIZE)
        assert_equal(sum(1 for m in members if m["valid"]), info["numValidMembers"])

        self.log.info("Membership is a selection, not the whole network")
        chosen = {m["proTxHash"] for m in members}
        assert_equal(len(chosen), Q60_SIZE)
        assert len(chosen) < MN_COUNT


if __name__ == "__main__":
    LLMQQ60DKGTest().main()
