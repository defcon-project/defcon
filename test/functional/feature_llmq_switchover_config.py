#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The startup guards on the Q60 switchover heights, without masternodes.

Both switchovers are two settings that must agree, and CheckLLMQConfiguration
(chainparams.cpp:1636-1683) refuses to start the node when they do not. This
test drives those refusals on regtest, where -testactivationheight is the only
way to set the heights, and it needs no quorum to do it: the checks run in
CreateChainParams before any block is read.

feature_llmq_q60_regtest.py and feature_llmq_q60_dkg.py cover the ChainLock
half being set correctly and forming; this covers the InstantSend half's name
and the pairing rule that ties it to the ChainLock height. It is cheap and
deterministic on a single node, so unlike the DKG test it belongs in the
runner.
"""

import re

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

CL = 480
IS_OK = 480          # equal is allowed: IS may ride the same height as CL
IS_BELOW = 360       # below CL is refused
IS_ALONE = 480       # IS set with no CL is refused


class LLMQSwitchoverConfigTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("instantsendv2 below chainlocksv2 is refused at startup")
        self.stop_node(0)
        # PARTIAL_REGEX matches with re.search, and the node prints the two
        # heights in parentheses -- unescaped they would be capture groups and
        # the assertion would pass on a message that never had them.
        node.assert_start_raises_init_error(
            ["-testactivationheight=chainlocksv2@%d" % CL,
             "-testactivationheight=instantsendv2@%d" % IS_BELOW],
            re.escape("nInstantSendV2ActivationHeight (%d) is below nChainLocksV2ActivationHeight (%d)"
                      % (IS_BELOW, CL)),
            match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("instantsendv2 without chainlocksv2 is refused: the profile is not registered")
        node.assert_start_raises_init_error(
            ["-testactivationheight=instantsendv2@%d" % IS_ALONE],
            "llmqTypeDIP0024InstantSendV2 names LLMQ type",
            match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("instantsendv2 at or above chainlocksv2 starts, and both heights take effect")
        self.start_node(0, extra_args=[
            "-testactivationheight=chainlocksv2@%d" % CL,
            "-testactivationheight=instantsendv2@%d" % IS_OK])
        # The node came up, which is the pairing rule passing. The profile the
        # two switchovers name is the same first-class Q60 profile, and it is
        # now registered and visible below its formation lead.
        assert "llmq_defcon" not in node.quorum("list")
        assert_equal(node.getblockcount(), 0)


if __name__ == "__main__":
    LLMQSwitchoverConfigTest().main()
