#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The Q60 ChainLock profile (llmq_defcon) on regtest.

Regtest registers llmq_defcon only when -testactivationheight=chainlocksv2@N
gives the switchover a height. Two reasons it is conditional, and this test
covers both: CheckLLMQConfiguration refuses a profile without a height to
switch at, and an unconditional registration would start a DKG handler for a
60-member profile in every regtest test, where the list is a handful of
masternodes and the session could only ever fail.

It also pins the formation lead, which is the fleet's rollout window and the
one number a mis-scheduled switchover would silently get wrong: quorums open
exactly (signingActiveQuorumCount + 1) * dkgInterval = 120 blocks before the
activation height, and not one block earlier.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

ACTIVATION = 240
FORMATION_LEAD = 120  # (signingActiveQuorumCount + 1) * dkgInterval = 5 * 24
# GetEnabledQuorumTypes tests pindexTip->nHeight + 1 >= activation - lead,
# so the tip height at which the profile first appears is one below the lead.
FIRST_ENABLED_TIP = ACTIVATION - FORMATION_LEAD - 1  # 119


class LLMQQ60RegtestTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-testactivationheight=chainlocksv2@%d" % ACTIVATION]]

    def quorum_types(self):
        return set(self.nodes[0].quorum("list").keys())

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Below the formation lead the profile is registered but not yet enabled")
        self.generate(node, FIRST_ENABLED_TIP - 1)
        assert_equal(node.getblockcount(), FIRST_ENABLED_TIP - 1)
        assert "llmq_defcon" not in self.quorum_types()

        self.log.info("It opens at exactly the lead, not a block earlier")
        self.generate(node, 1)
        assert_equal(node.getblockcount(), FIRST_ENABLED_TIP)
        assert "llmq_defcon" in self.quorum_types()

        self.log.info("It stays enabled across the activation height itself")
        self.generate(node, ACTIVATION - node.getblockcount())
        assert_equal(node.getblockcount(), ACTIVATION)
        assert "llmq_defcon" in self.quorum_types()

        self.log.info("Without the argument regtest never registers it, at any height")
        self.restart_node(0, extra_args=[])
        assert_equal(node.getblockcount(), ACTIVATION)
        assert "llmq_defcon" not in self.quorum_types()

        self.log.info("A node that names the switchover starts, and reports the height it was given")
        self.restart_node(0, extra_args=["-testactivationheight=chainlocksv2@%d" % ACTIVATION])
        assert "llmq_defcon" in self.quorum_types()


if __name__ == "__main__":
    LLMQQ60RegtestTest().main()
