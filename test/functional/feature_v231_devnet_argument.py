#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""-v231activationheight on a devnet daemon: the rehearsal needs no rebuilt binary.

The daemon knows the argument and starts with a height the bundle can take. It refuses
to start with one it cannot: off the 576-block grid, at or below the Q60 switchover, below
the proof-of-stake coinbase bound, or not a usable number. What the height then does is
held by chainparams_v23_bundle_tests on these parameters and by feature_v231_transition.py
on a chain: a devnet cannot reach the height here, because it stakes on an unmockable clock.
"""

import re

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

H = 8064    # the lowest it can take: 14 * 576, the first multiple at or above this devnet's coinbase bound at 7560
TIP = 1     # a devnet starts on its own genesis block, which sits at height 1


class V231DevnetArgumentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = "devnet"
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        assert_equal(node.getblockcount(), TIP)
        self.stop_node(0)

        self.log.info("The daemon takes a height on the grid above the switchover")
        with node.assert_debug_log(["Setting v231activationheight to %d" % H]):
            self.start_node(0, extra_args=["-v231activationheight=%d" % H])
        assert_equal(node.getblockcount(), TIP)
        self.stop_node(0)

        self.log.info("And refuses to start with one it cannot schedule")
        for value, reason in ((H + 1, "is not on its DKG grid"),
                              (H + 72, "is not on its DKG grid"),
                              (H - 576, "must not precede the proof-of-stake coinbase bound"),
                              (2880, "must follow the Q60 ChainLocks switchover"),
                              ("0", "Invalid value of v231activationheight (0)"),
                              ("abc", "Invalid value of v231activationheight (abc)"),
                              ("%dx" % H, "Invalid value of v231activationheight (%dx)" % H),
                              ("2147483647", "Invalid value of v231activationheight (2147483647)")):
            node.assert_start_raises_init_error(extra_args=["-v231activationheight=%s" % value],
                                                expected_msg=re.escape(reason), match=ErrorMatch.PARTIAL_REGEX)

        self.log.info("The same data directory starts again without it")
        self.start_node(0)
        assert_equal(node.getblockcount(), TIP)


if __name__ == '__main__':
    V231DevnetArgumentTest().main()
