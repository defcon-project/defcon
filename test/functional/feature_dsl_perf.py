#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sentinel probes and actual commitments still work with bounded diagnostics.

Use a complete healthy pool and mine a real commitment. Epoch summaries must
show real verification and cache reuse; no per-message metric log is required.
"""

import re

from feature_dsl_service import CUTOFF, EPOCH_INTERVAL, DSLServiceTest
from test_framework.util import assert_equal, force_finish_mnsync


class DSLPerfTest(DSLServiceTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [[*args, "-dslperf=1"] for args in self.extra_args]

    def run_test(self):
        producer = self.nodes[0]
        producer.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        for peer in self.nodes:
            force_finish_mnsync(peer)
        self.mine_quorum()
        self.bump_mocktime(60)
        self.generate(producer, EPOCH_INTERVAL - producer.getblockcount() % EPOCH_INTERVAL)
        epoch = producer.getblockcount() // EPOCH_INTERVAL
        def current_liveness():
            status = [peer.dslstatus() for peer in self.nodes]
            return all(item["epoch"] == epoch and item["respondedcount"] == len(self.mninfo) for item in status)

        self.wait_until(current_liveness, timeout=60)
        self.bump_mocktime(1)
        self.generate(producer, CUTOFF)

        def complete_pool():
            status = [peer.dslstatus() for peer in self.nodes]
            return (all(item["epoch"] == epoch and item["epochreports"] == len(self.mninfo) * (len(self.mninfo) - 1) for item in status)
                    and len({item["poolhash"] for item in status}) == 1)

        # This measurement fixture needs identical, complete snapshots, rather
        # than coupling its readiness to a randomly selected offline signer.
        # The independent feature_dsl_service test covers the outage case.
        self.wait_until(complete_pool, timeout=60)
        for peer in self.nodes:
            assert_equal(peer.dslstatus()["missedreports"], 0)
        self.generate(producer, EPOCH_INTERVAL - CUTOFF - 3)  # first signing tick, +21
        self.wait_until(lambda: self.has_signature(producer, epoch), timeout=60)
        self.generate(producer, 3)
        block = producer.getblock(producer.getbestblockhash(), 2)
        commitments = [tx["poseServiceTx"]["commitment"] for tx in block["tx"] if tx.get("type") == 10]
        assert_equal(len(commitments), 1)
        assert_equal(commitments[0]["epoch"], epoch)
        assert_equal(commitments[0]["missedCount"], 0)

        def summaries():
            return [line for line in producer.debug_log_path.read_text(encoding="utf-8").splitlines()
                    if "DSL perf -- epoch=" in line]

        self.wait_until(lambda: any(re.search(r"cache_hit=[1-9][0-9]*/", line) for line in summaries()), timeout=30)
        lines = summaries()
        assert any(re.search(r"bls_verify=[1-9][0-9]*/", line) for line in lines)
        assert any(re.search(r"rpc_build=[1-9][0-9]*/", line) for line in lines)
        assert any(re.search(r"tip_queue=[1-9][0-9]*/", line) for line in lines)
        assert "DSL perf -- measurement overflow=" not in producer.debug_log_path.read_text(encoding="utf-8")
        self.log.info("Identical complete pools, real mined commitment and bounded BLS reuse diagnostics passed")


if __name__ == '__main__':
    DSLPerfTest().main()
