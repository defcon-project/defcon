#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Injected DSL faults acting on a running masternode, on regtest.

Seven masternodes probe each other with DSL active from genesis and fault
injection enabled on every node. A healthy epoch is the control: every report
at the cutoff is ONLINE. Then a `response-drop` fault on one *running*
masternode makes its sentinels report it MISSED, while the fault counts what
it withheld; clearing the fault before the next epoch has the node reported
ONLINE again, so recovery is proven without a restart. Finally a
`report-drop` on another masternode leaves that node's reports out of its
peers' pools. Nothing here touches consensus; what is observed is the
off-chain pool, exactly as in feature_dsl_service.py.
"""

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync

EPOCH_INTERVAL = 24
CUTOFF = EPOCH_INTERVAL - EPOCH_INTERVAL // 4  # reports are emitted from this offset
ARGS = ["-testactivationheight=dsl@1", "-enablefaultinjection=1"]


class DSLFaultsTest(DashTestFramework):
    def set_test_params(self):
        # The same population as feature_dsl_service.py: one node out of seven
        # is 14.3%, under the 15% mass-outage guard, and its sentinels are the
        # six others, enough for the aggregation threshold.
        self.set_dash_test_params(8, 7, extra_args=[ARGS] * 8)

    def enter_next_epoch(self, node, expected_responders):
        # enter the epoch, then let the flood settle before judging it
        height = node.getblockcount()
        self.bump_mocktime(60)
        self.generate(node, EPOCH_INTERVAL - height % EPOCH_INTERVAL)
        self.wait_until(lambda: node.dslstatus()["respondedcount"] == expected_responders, timeout=60)

    def walk_to_cutoff(self, node):
        self.bump_mocktime(30)
        self.generate(node, CUTOFF)
        self.wait_until(lambda: node.dslstatus()["epochreports"] > 0, timeout=90)
        # the pool is complete once it has stopped growing: every masternode
        # emits at the same cutoff block and the flood settles within seconds
        stable_since = time.time()
        last = node.dslstatus()["epochreports"]
        deadline = time.time() + 90
        while time.time() < deadline:
            time.sleep(1)
            now = node.dslstatus()["epochreports"]
            if now != last:
                last = now
                stable_since = time.time()
            elif time.time() - stable_since >= 8:
                break
        return node.dslstatus()

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)
        mn_count = len(self.mninfo)

        self.log.info("Control: a healthy epoch pools only ONLINE reports")
        self.enter_next_epoch(node, mn_count)
        healthy = self.walk_to_cutoff(node)
        assert_equal(healthy["missedreports"], 0)
        assert_greater_than(healthy["onlinereports"], 0)
        healthy_pool = healthy["epochreports"]

        self.log.info("A response-drop on a running masternode: its sentinels report it MISSED")
        target = self.mninfo[0]
        tnode = target.node
        far = node.getblockcount() + 10 * EPOCH_INTERVAL
        fault = tnode.faultinject("set", "response-drop", far, "day19-response-drop")
        assert_equal(fault["hits"], 0)
        self.enter_next_epoch(node, mn_count - 1)
        assert not node.dslstatus()["respondedcount"] == mn_count, "the faulted node announced anyway"
        faulted = self.walk_to_cutoff(node)
        assert_greater_than(faulted["missedreports"], 0)
        hits = [f for f in tnode.faultinject("list")["faults"] if f["id"] == fault["id"]][0]["hits"]
        assert_greater_than(hits, 0)
        self.log.info("  fault %d held %d announcement(s); pool: %d online, %d missed",
                      fault["id"], hits, faulted["onlinereports"], faulted["missedreports"])

        self.log.info("Cleared before the next epoch, the same node is ONLINE again: recovery without a restart")
        assert_equal(tnode.faultinject("clear", fault["id"])["cleared"], 1)
        self.enter_next_epoch(node, mn_count)
        recovered = self.walk_to_cutoff(node)
        assert_equal(recovered["missedreports"], 0)
        assert_equal(recovered["epochreports"], healthy_pool)

        self.log.info("A report-drop leaves that masternode's reports out of its peers' pools")
        reporter = self.mninfo[1].node
        fault2 = reporter.faultinject("set", "report-drop", node.getblockcount() + 10 * EPOCH_INTERVAL, "day19-report-drop")
        self.enter_next_epoch(node, mn_count)
        dropped = self.walk_to_cutoff(node)
        assert_greater_than(healthy_pool, dropped["epochreports"])
        hits2 = [f for f in reporter.faultinject("list")["faults"] if f["id"] == fault2["id"]][0]["hits"]
        assert_greater_than(hits2, 0)
        # and nobody was reported MISSED: the reporter is silent, not its targets
        assert_equal(dropped["missedreports"], 0)
        self.log.info("  fault %d held the reports; pool %d -> %d", fault2["id"], healthy_pool, dropped["epochreports"])

        self.log.info("Cleared, the pool is whole again")
        assert_equal(reporter.faultinject("clear")["cleared"], 1)
        self.enter_next_epoch(node, mn_count)
        whole = self.walk_to_cutoff(node)
        assert_equal(whole["epochreports"], healthy_pool)
        assert_equal(whole["missedreports"], 0)


if __name__ == '__main__':
    DSLFaultsTest().main()
