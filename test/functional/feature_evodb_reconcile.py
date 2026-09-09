#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""An evodb that stopped short of the coins tip is rebuilt at startup.

The evodb reaches disk on one path only, CEvoDB::CommitRootTransaction, and only
a full flush calls it. A process that dies during a long import therefore leaves
the coins database at its tip and the evodb wherever its last commit left it --
at nothing at all, if -reindex wiped it and no commit followed.
CDeterministicMNManager::MigrateDBIfNeeded then reads the absent marker as an
interrupted migration and the node refuses to start, with a full reindex as the
documented way out.

ReconcileEvoDBToTip closes that gap before the migration gate runs. This test
holds it to the strict criterion: not that the node starts, but that it comes up
at the same tip AND that the rebuilt state equals the state that was there
before -- masternode list, quorum list and the diff ladder, compared byte for
byte.

The masternodes are what make that comparison mean anything. An empty list
equals an empty list, so a reconciliation that built nothing at all would pass a
comparison taken on a bare chain; the run asserts up front that the reference
actually carries masternodes and quorums.
"""

import shutil

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than


class EvoDBReconcileTest(DashTestFramework):
    def set_test_params(self):
        # Four nodes, three of them masternodes: enough for the framework's
        # llmq_test profile (size 3, threshold 2) to form a quorum, so the
        # replay has quorum commitments to rebuild and not only list diffs.
        self.set_dash_test_params(4, 3)

    def evodb_dir(self, node):
        return node.chain_path / "evodb"

    def log_since(self, node, offset):
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(offset)
            return f.read()

    def log_size(self, node):
        return node.debug_log_path.stat().st_size

    def snapshot(self, node, tip_height):
        """Everything the replay is responsible for rebuilding.

        The diff ladder is the discriminating part: `protx diff 1 H` at several
        heights is answered from the evodb, so a rebuild that produced the wrong
        list -- or no list -- differs here even when the tip height matches.
        """
        ladder = {}
        for height in range(1, tip_height + 1, max(1, tip_height // 8)):
            ladder[height] = node.protx("diff", 1, height)
        ladder[tip_height] = node.protx("diff", 1, tip_height)
        return {
            "ladder": ladder,
            "registered": node.protx("list", "registered", True),
            "mn_count": node.masternode("count"),
            "quorums": node.quorum("list"),
        }

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mine a quorum so the evodb holds commitments as well as list diffs")
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.mine_quorum()

        tip_height = node.getblockcount()
        tip_hash = node.getbestblockhash()
        reference = self.snapshot(node, tip_height)

        self.log.info("The comparison must be able to discriminate")
        # Without this the whole test is an empty-equals-empty tautology, which
        # is exactly how an earlier hand-run of this scenario passed on a fix
        # that rebuilt nothing.
        assert_greater_than(len(reference["registered"]), 0)
        assert_equal(reference["mn_count"]["enabled"], self.mn_count)
        assert_greater_than(sum(len(q) for q in reference["quorums"].values()), 0)
        non_empty = sum(1 for d in reference["ladder"].values() if len(d["mnList"]) > 0)
        assert_greater_than(non_empty, 0)
        self.log.info("  reference: %d registered, %d ladder heights carry a list"
                      % (len(reference["registered"]), non_empty))

        self.log.info("Stop the node and remove its evodb entirely")
        self.stop_node(0)
        evodb = self.evodb_dir(node)
        assert evodb.is_dir()
        shutil.rmtree(evodb)
        assert not evodb.exists()

        self.log.info("It must start, replay, and come back to the same tip")
        offset = self.log_size(node)
        self.start_node(0)
        self.connect_nodes(0, 1)
        self.sync_blocks()

        log = self.log_since(node, offset)
        assert "ReconcileEvoDBToTip" in log
        assert "replaying" in log, "the reconciliation did not run; the recovery is not attributable to it"
        assert "Error upgrading Evo database" not in log
        assert "Found EvoDB inconsistency" not in log
        assert "bad-qc-not-allowed" not in log

        assert_equal(node.getblockcount(), tip_height)
        assert_equal(node.getbestblockhash(), tip_hash)

        self.log.info("...and the rebuilt state must equal the state that was there")
        rebuilt = self.snapshot(node, tip_height)
        assert_equal(rebuilt["registered"], reference["registered"])
        assert_equal(rebuilt["mn_count"], reference["mn_count"])
        assert_equal(rebuilt["quorums"], reference["quorums"])
        for height, diff in reference["ladder"].items():
            assert_equal(rebuilt["ladder"][height], diff)

        self.log.info("A second clean restart must not replay again: the marker is durable")
        self.stop_node(0)
        offset = self.log_size(node)
        self.start_node(0)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        log = self.log_since(node, offset)
        assert "replaying" not in log, "the reconciliation ran a second time; the marker was not written"
        assert_equal(node.getblockcount(), tip_height)


if __name__ == '__main__':
    EvoDBReconcileTest().main()
