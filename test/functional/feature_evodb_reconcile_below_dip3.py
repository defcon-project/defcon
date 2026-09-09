#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Below DIP0003 an empty evodb is current, not lagging.

A node killed early in initial sync, before its first full flush, has a coins
database at some height and an evodb with nothing in it. Below DIP0003 that is
not a gap: there is no deterministic masternode list yet, so empty is the
correct content and there is nothing to replay.

Returning early is not enough, and this is the part worth a regression test. On
an empty database CDeterministicMNManager::MigrationAlreadyDone is false, so the
migration gate never reaches its own below-DIP3 branch -- the one that writes
the best-block marker -- and fails four lines earlier on its
"b_b2 is gone, so a previous migration was interrupted" test. The node then
refuses to start with "Error upgrading Evo database", even though the
reconciliation correctly decided to do nothing.

So the reconciliation writes the marker itself, and this test fails if that is
ever removed: the node would come up on the first arm and refuse on the second.

This test uses the plain framework rather than DashTestFramework because the
latter pins dip3params to 2:2, which puts DIP3 out of reach at height 2. Here
regtest's own DIP0003Height (432) applies.
"""

import shutil

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# chainparams.cpp, CRegTestParams
DIP0003_HEIGHT = 432


class EvoDBReconcileBelowDIP3Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def evodb_dir(self, node):
        return node.chain_path / "evodb"

    def log_since(self, node, offset):
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(offset)
            return f.read()

    def wipe_evodb_and_restart(self, node):
        """Stop, remove the evodb entirely, start again. Returns the new log."""
        self.stop_node(0)
        evodb = self.evodb_dir(node)
        assert evodb.is_dir()
        shutil.rmtree(evodb)
        assert not evodb.exists()
        offset = node.debug_log_path.stat().st_size
        self.start_node(0)
        return self.log_since(node, offset)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("A chain that has not reached DIP0003 yet")
        self.generate(node, 100)
        height = node.getblockcount()
        assert_equal(height, 100)
        assert height < DIP0003_HEIGHT

        self.log.info("With the evodb gone, the node must still start")
        log = self.wipe_evodb_and_restart(node)
        assert_equal(node.getblockcount(), height)
        assert "below DIP0003" in log, "the below-DIP3 branch did not run"
        assert "marked it current" in log, "the marker was not written"
        assert "Error upgrading Evo database" not in log, \
            "the migration gate stranded the node -- the marker write is what prevents this"
        assert "replaying" not in log, "there is nothing to replay below DIP0003"

        self.log.info("The marker must be durable: a second restart replays nothing either")
        self.stop_node(0)
        offset = node.debug_log_path.stat().st_size
        self.start_node(0)
        log = self.log_since(node, offset)
        assert "Error upgrading Evo database" not in log
        assert_equal(node.getblockcount(), height)

        self.log.info("Past DIP0003 the same wipe takes the replay path instead")
        self.generate(node, DIP0003_HEIGHT + 20 - height)
        height = node.getblockcount()
        assert height > DIP0003_HEIGHT
        log = self.wipe_evodb_and_restart(node)
        assert_equal(node.getblockcount(), height)
        assert "replaying" in log, "above DIP0003 the replay must run"
        assert "below DIP0003" not in log
        assert "Error upgrading Evo database" not in log
        assert "Found EvoDB inconsistency" not in log


if __name__ == '__main__':
    EvoDBReconcileBelowDIP3Test().main()
