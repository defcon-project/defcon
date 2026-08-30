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

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
CUTOFF = EPOCH_INTERVAL - EPOCH_INTERVAL // 4  # reports are emitted from this offset


class DSLServiceTest(DashTestFramework):
    def set_test_params(self):
        # Seven masternodes, for two thresholds at once: a stopped node's
        # sentinels are all the others, and the aggregation threshold
        # (nDSLSentinelAgree = 5) needs five of them to agree before its MISSED
        # bit can set -- and one node out of seven is 14.3%, just under the
        # mass-outage guard (15%), so the missed-epoch counter actually
        # advances instead of the guard freezing the epoch.
        self.extra_args = [["-testactivationheight=dsl@1"]] * 8
        self.set_dash_test_params(8, 7, extra_args=self.extra_args)

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
        # enter the next epoch first and let the survivors' announcements flood
        # before the cutoff -- a block burst straight to the cutoff would give
        # the flood no time and make everyone look missed
        height = node.getblockcount()
        self.bump_mocktime(300, nodes=alive)
        self.generate(node, EPOCH_INTERVAL - height % EPOCH_INTERVAL, sync_fun=lambda: self.sync_blocks(alive))
        self.wait_until(lambda: node.dslstatus()["respondedcount"] == len(self.mninfo) - 1, timeout=60)
        self.bump_mocktime(60, nodes=alive)
        self.generate(node, CUTOFF, sync_fun=lambda: self.sync_blocks(alive))
        self.wait_until(lambda: node.dslstatus()["missedreports"] > 0 and node.dslstatus()["onlinereports"] > 0,
                        timeout=90)

        self.log.info("Restarting the stopped masternode and forming the attesting quorum")
        self.start_masternode(stopped)
        # the restarted node came back with the mocktime it was stopped at, and
        # headers arriving before its clock catches up read as time-too-new --
        # which costs it its only peer. Clock first, connection second.
        self.bump_mocktime(1)
        self.connect_nodes(stopped.nodeIdx, 0)
        self.sync_blocks()
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.mine_quorum()

        self.log.info("A commitment lands on an epoch boundary and applies in shadow")
        stopped_protx = stopped.proTxHash
        self.stop_node(stopped.nodeIdx)
        alive = [n for i, n in enumerate(self.nodes) if i != stopped.nodeIdx]

        found_commitment = False
        for _ in range(4):  # observe up to four epochs
            # phase the epoch honestly: enter it, let announcements flood, walk
            # to the cutoff so reports pool, pause at the signing offset so the
            # quorum's threshold signature can recover, then cross the boundary
            height = node.getblockcount()
            self.bump_mocktime(60, nodes=alive)
            self.generate(node, EPOCH_INTERVAL - (height % EPOCH_INTERVAL), sync_fun=lambda: self.sync_blocks(alive))
            self.wait_until(lambda: node.dslstatus()["respondedcount"] == len(self.mninfo) - 1, timeout=60)
            self.bump_mocktime(30, nodes=alive)
            self.generate(node, CUTOFF, sync_fun=lambda: self.sync_blocks(alive))
            self.wait_until(lambda: node.dslstatus()["missedreports"] > 0, timeout=60)
            self.generate(node, EPOCH_INTERVAL - CUTOFF - 2, sync_fun=lambda: self.sync_blocks(alive))
            self.bump_mocktime(10, nodes=alive)
            time.sleep(3)
            self.generate(node, 2, sync_fun=lambda: self.sync_blocks(alive))
            block = node.getblock(node.getbestblockhash(), 2)
            dsl_txs = [tx for tx in block["tx"] if tx.get("type") == 10]
            if dsl_txs:
                found_commitment = True
                break
        assert found_commitment, "no service commitment was mined within four epochs"

        info = node.protx("info", stopped_protx)
        assert info["state"]["missedServiceEpochs"] >= 1
        # shadow mode records, but never penalises
        assert_equal(info["state"]["rewardSuspended"], False)
        assert_equal(info["state"]["dslBanHeight"], -1)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLServiceTest().main()
