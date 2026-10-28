#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A repair asked for with -evodbrepair finishes before the node starts serving.

Masternode lists are rebuilt from the stored diffs, so a repair of those diffs
has to be done before anything reads them. It runs while the chainstate loads,
before the block verification that follows (which disconnects blocks and reads
the lists before them), the block import thread, the network threads, the
staking thread and the end of RPC warmup; the order of the debug log shows it.
Run in the background after the import, as it used to, it raced all of them --
including the import itself, which reads the masternode lists of older quorums.

A repair that completes marks the history repaired, so the next start with
-evodbrepair skips it, unless -forceevodbrepair asks again.

The plain framework keeps regtest's own DIP0003Height (432); DashTestFramework
would pin it to 2.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Past DIP0003 (432) and the first list snapshot (576), so there is a snapshot
# pair to check.
CHAIN_HEIGHT = 600
REPAIR_START = "Repairing masternode list diffs..."
REPAIR_DONE = "Verified 1 snapshot pair(s)"
# The verification-only pass the import thread runs when no repair was asked for.
VERIFY_START = "Verifying masternode list diffs..."


class EvoDBRepairStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def log_since(self, node, offset):
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(offset)
            return f.read()

    def restart_with(self, extra_args):
        node = self.nodes[0]
        self.stop_node(0)
        offset = node.debug_log_path.stat().st_size
        self.start_node(0, extra_args=extra_args)
        return self.log_since(node, offset)

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, CHAIN_HEIGHT)

        self.log.info("-evodbrepair runs and finishes before the import, the network and the end of warmup")
        log = self.restart_with(["-evodbrepair"])
        done = log.find(REPAIR_DONE)
        assert done != -1, "the requested repair did not finish clean"
        for later in ("init message: Verifying blocks", "loadblk thread start", "net thread start",
                      "msghand thread start", "init message: Done loading"):
            position = log.find(later)
            assert position != -1, f"{later!r} is missing from the log"
            assert done < position, f"{later!r} came before the repair finished"
        staking = log.find("threadstakeminer thread start")
        assert staking == -1 or done < staking, "the staking thread started before the repair finished"
        start = log.find(REPAIR_START)
        assert start != -1 and start < done, "the repair finished without starting as a repair"
        # Clean, it marked the history, so the import thread does not check it again.
        assert VERIFY_START not in log
        assert "Masternode list diffs are already repaired" in log[done:]
        assert_equal(node.getblockcount(), CHAIN_HEIGHT)

        self.log.info("A completed repair is remembered")
        log = self.restart_with(["-evodbrepair"])
        assert REPAIR_START not in log and VERIFY_START not in log
        assert "Masternode list diffs are already repaired" in log

        self.log.info("-forceevodbrepair asks again, still at load")
        log = self.restart_with(["-evodbrepair", "-forceevodbrepair"])
        done = log.find(REPAIR_DONE)
        assert log.find(REPAIR_START) != -1 and done != -1
        assert done < log.find("loadblk thread start")
        assert VERIFY_START not in log
        assert "Masternode list diffs were checked by the repair at load" in log
        assert_equal(node.getblockcount(), CHAIN_HEIGHT)


if __name__ == '__main__':
    EvoDBRepairStartupTest().main()
