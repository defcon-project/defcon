#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The DeFCon Sentinel Layer probe end to end on regtest.

Every masternode announces its own liveness once per epoch and the announcement
floods the mesh; at the epoch cutoff each masternode signs a report for every
target it was assigned to watch -- ONLINE for one it heard from, MISSED for one
it did not. This test activates DSL from genesis, watches a healthy epoch pool
nothing but ONLINE reports, then stops one masternode and watches the next
epoch's cutoff report it MISSED. Everything observed here is the off-chain
pool; no consensus rule is exercised.
"""

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
CUTOFF = EPOCH_INTERVAL - EPOCH_INTERVAL // 4  # reports are emitted from this offset


class DSLServiceTest(DashTestFramework):
    def set_test_params(self):
        self.extra_args = [["-testactivationheight=dsl@1"]] * 5
        self.set_dash_test_params(5, 4, extra_args=self.extra_args)

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        self.log.info("Crossing an epoch boundary so every masternode announces afresh")
        height = node.getblockcount()
        self.bump_mocktime(1)
        self.generate(node, EPOCH_INTERVAL - (height % EPOCH_INTERVAL))

        self.log.info("Every masternode's liveness announcement reaches this node")
        self.wait_until(lambda: node.dslstatus()["respondedcount"] == len(self.mninfo), timeout=60)
        assert node.dslstatus()["active"]

        self.log.info("At the cutoff the sentinels file reports, and nobody is missed")
        pos = node.getblockcount() % EPOCH_INTERVAL
        if pos < CUTOFF:
            self.bump_mocktime(1)
            self.generate(node, CUTOFF - pos)
        self.wait_until(lambda: node.dslstatus()["epochreports"] > 0, timeout=60)
        status = node.dslstatus()
        assert_equal(status["missedreports"], 0)
        assert status["onlinereports"] > 0

        self.log.info("Stopping one masternode")
        stopped = self.mninfo[0]
        self.stop_node(stopped.nodeIdx)
        alive = [n for i, n in enumerate(self.nodes) if i != stopped.nodeIdx]

        self.log.info("The next epoch's cutoff reports it MISSED")
        height = node.getblockcount()
        to_next_cutoff = (EPOCH_INTERVAL - height % EPOCH_INTERVAL) + CUTOFF
        self.bump_mocktime(300, nodes=alive)
        self.generate(node, to_next_cutoff, sync_fun=lambda: self.sync_blocks(alive))
        self.wait_until(lambda: node.dslstatus()["missedreports"] > 0, timeout=90)
        status = node.dslstatus()
        assert_equal(status["respondedcount"], len(self.mninfo) - 1)
        assert status["onlinereports"] > 0

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLServiceTest().main()
