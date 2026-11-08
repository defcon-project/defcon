#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sentinel probes and actual commitments still work with bounded diagnostics.

Use a complete healthy pool and mine a real commitment. Epoch summaries must
show real verification and cache reuse; no per-message metric log is required.
They also carry the latency histograms per DKG-cycle position, which
contrib/devtools/dsl_perf_pct.py reads, and the test switch that slows a node
down refuses a delay out of range at startup.
"""

import json
import os
import re
import subprocess
import sys

from feature_dsl_service import CUTOFF, EPOCH_INTERVAL, DSLServiceTest
from test_framework.util import assert_equal, force_finish_mnsync


class DSLPerfTest(DSLServiceTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [[*args, "-dslperf=1"] for args in self.extra_args]

    def run_test(self):
        self.check_percentile_bounds()
        self.run_perf_checks()
        self.check_startup_refusal()

    def check_percentile_bounds(self):
        """contrib/devtools/dsl_perf_pct.py on fixed histograms: the overflow samples are the largest ones, so
        only a rank among them is a lower bound; and the text never rounds a bound past its exact value."""
        script = os.path.join(self.config["environment"]["SRCDIR"], "contrib", "devtools", "dsl_perf_pct.py")
        path = os.path.join(self.options.tmpdir, "dsl_perf_pct_bounds.log")
        base = "00" * 32
        with open(path, "w", encoding="utf-8") as f:
            for pos, n, over, buckets in ((0, 100, 1, "49:100"), (1, 100, 2, "49:100"), (2, 10, 1, "20:5,49:5"),
                                          (3, 1, 0, "25:1"), (4, 1, 0, "10:1")):
                f.write(f"DSL perf hist -- epoch=1 base={base} metric=tip_queue pos={pos} n={n} over={over} buckets={buckets}\n")
        rows = json.loads(subprocess.run([sys.executable, script, "--json", path], capture_output=True, text=True, check=True).stdout)
        by_pos = {row["pos"]: row for row in rows}
        def bounds(pos):
            return [(by_pos[pos][f"{q}_ns"], by_pos[pos][f"{q}_lower_bound"]) for q in ("p50", "p90", "p99")]
        top = 2 ** 50
        # 99 samples below 2^50 ns and one above: p99 (rank 99) is still one below, so every bound is an upper one.
        assert_equal(bounds(0), [(top, False), (top, False), (top, False)])
        # Two above: rank 99 of 100 is one of them, known only to be at least 2^50 ns.
        assert_equal(bounds(1), [(top, False), (top, False), (top, True)])
        # Shared top bucket: p50 in bucket 20, p90 (rank 9) the last sample below 2^50 ns, p99 (rank 10) above.
        assert_equal(bounds(2), [(2 ** 21, False), (top, False), (top, True)])
        text = subprocess.run([sys.executable, script, path], capture_output=True, text=True, check=True).stdout
        rows_text = {int(line.split()[1]): line.split()[3:] for line in text.splitlines()[1:]}
        assert_equal(rows_text[3], ["<=67.2ms"] * 3)  # 2^26 ns is 67.108864 ms: rounded up, not to 67.1
        assert_equal(rows_text[4], ["<=2.05us"] * 3)  # 2^11 ns is 2.048 us
        # 2^50 ns is 1125899.906842624 s: as a lower bound rounded down, as an upper bound up.
        assert_equal(rows_text[1][2], ">=1125899s")
        assert_equal(rows_text[0][2], "<=1125900s")
        self.log.info("The percentile script's bounds hold where a bucket is shared and in its text")

    def run_perf_checks(self):
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
        assert any(re.search(r"decode_response=[1-9][0-9]*/", line) for line in lines)
        assert "DSL perf -- measurement overflow=" not in producer.debug_log_path.read_text(encoding="utf-8")
        self.log.info("Identical complete pools, real mined commitment and bounded BLS reuse diagnostics passed")

        def histograms(node):
            return [line for line in node.debug_log_path.read_text(encoding="utf-8").splitlines()
                    if "DSL perf hist -- epoch=" in line]

        # The producer runs every tip callback; a node that receives its blocks also waits for block messages.
        receiver = self.nodes[1]
        self.wait_until(lambda: any(" metric=block_msg_wait " in line for line in histograms(receiver)), timeout=30)
        for metric in ("tip_queue", "peerman_tip_run", "dkg_tip_start", "dsl_tick_run"):
            assert any(f" metric={metric} " in line for line in histograms(producer)), metric
        script = os.path.join(self.config["environment"]["SRCDIR"], "contrib", "devtools", "dsl_perf_pct.py")
        for node, metric in ((producer, "dsl_tick_run"), (receiver, "block_msg_wait")):
            result = subprocess.run([sys.executable, script, "--json", "--metric", metric, str(node.debug_log_path)],
                                    capture_output=True, text=True, check=True)
            rows = json.loads(result.stdout)
            assert rows and all(row["metric"] == metric for row in rows)
            for row in rows:
                assert 0 <= row["pos"] < EPOCH_INTERVAL
                assert row["n"] > 0
                assert row["p50_ns"] <= row["p90_ns"] <= row["p99_ns"]
        self.log.info("Per-position latency histograms are written and read back as percentiles")

    def check_startup_refusal(self):
        # Last, and not in the subclasses that go on using node 0 (feature_dsl_perf_burst.py).
        producer = self.nodes[0]
        self.stop_node(0)
        producer.assert_start_raises_init_error(
            extra_args=["-dsltestprocessdelay=-1"],
            expected_msg="Error: -dsltestprocessdelay must be between 0 and 10000 milliseconds")
        self.log.info("A test process delay out of range stops startup")


if __name__ == '__main__':
    DSLPerfTest().main()
