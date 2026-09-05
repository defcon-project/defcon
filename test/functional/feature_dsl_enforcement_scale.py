#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The Sentinel Layer's punishing branch at a network size the simulator can meet.

feature_dsl_enforcement.py drives the same path at seven masternodes. Seven is
enough to reach the rule, but the sentinel selection there is degenerate: a
target's seven sentinels are simply the six other nodes, so nothing is being
selected. It also cannot be compared against the scale simulator, whose DKG-PoSe
track needs a 60-member quorum and so refuses any population below 60.

Sixty-five is the smallest size where both are true at once: selection draws 7
sentinels out of 64, and the simulator will run. That makes this the one point
where the two methods measure the same quantity on the same rule, which is what
lets the simulator's 150-15000 range be believed. The simulator says a stopped
masternode is banned after exactly nDSLBanEpochs epochs at every population it
covers; this asserts the same number with real BLS keys, real gossip and a real
mined commitment.

Everything else -- the epoch walk, the assertions, the revive -- is inherited
unchanged, so any divergence is the network size and nothing else.
"""

import os

# Both of these need more daemons than the framework's historical 20-node
# ceiling, and MAX_NODES is read at import time -- so it has to be set before
# test_framework comes in. Set here rather than in the caller's environment so
# the test is runnable on its own. Not in test_runner.py's list: a job with a
# different ceiling gets a different port stride, and two of those in parallel
# can collide.
os.environ.setdefault("TEST_RUNNER_MAX_NODES", "160")

from feature_dsl_enforcement import DSLEnforcementTest

MN_COUNT = 65


class DSLEnforcementScaleTest(DSLEnforcementTest):
    def set_test_params(self):
        # One stopped node is 1/65 = 1.5% of the network, far below the 15%
        # mass-outage guard, so the missed-epoch counter advances rather than
        # the guard freezing the epoch -- the same condition the seven-node
        # test arranges, with far more headroom.
        self.extra_args = [[
            "-testactivationheight=dsl@1",
            "-testactivationheight=dslenforcement@1",
        ]] * (MN_COUNT + 1)
        self.set_dash_test_params(MN_COUNT + 1, MN_COUNT, extra_args=self.extra_args)

    def run_test(self):
        # BitcoinTestMetaClass requires every subclass to declare both
        # set_test_params and run_test. The whole point here is that the body is
        # the seven-node test's, unchanged -- so declare it and delegate.
        super().run_test()


if __name__ == '__main__':
    DSLEnforcementScaleTest().main()
