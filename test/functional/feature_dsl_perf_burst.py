#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Real InstantSend and ChainLock progress during a Sentinel cutoff burst.

Use the non-rotating regtest quorum for both locks, as the live chain does.
Latency is measured with a wall-clock stopwatch, independently of mocktime.
"""

import time

from feature_dsl_perf import DSLPerfTest
from feature_dsl_service import CUTOFF, EPOCH_INTERVAL
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, force_finish_mnsync


class DSLPerfBurstTest(DSLPerfTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [[*args, "-llmqtestinstantsenddip0024=llmq_test"] for args in self.extra_args]

    def run_test(self):
        super().run_test()
        node = self.nodes[0]
        for peer in self.nodes:
            force_finish_mnsync(peer)
        for spork in ("SPORK_2_INSTANTSEND_ENABLED", "SPORK_3_INSTANTSEND_BLOCK_FILTERING", "SPORK_19_CHAINLOCKS_ENABLED"):
            node.sporkupdate(spork, 0)
        self.wait_for_sporks_same()
        receiver = self.nodes[1]

        def locked(txids):
            self.bump_mocktime(1)
            try:
                return all(receiver.getrawtransaction(txid, True)["instantlock_internal"] for txid in txids)
            except JSONRPCException:
                return False

        self.bump_mocktime(60)
        self.generate(node, EPOCH_INTERVAL - node.getblockcount() % EPOCH_INTERVAL)
        epoch = node.getblockcount() // EPOCH_INTERVAL

        def current_liveness():
            status = [peer.dslstatus() for peer in self.nodes]
            return all(item["epoch"] == epoch and item["respondedcount"] == len(self.mninfo) for item in status)

        self.wait_until(current_liveness, timeout=60)
        start = time.monotonic()
        baseline = node.sendtoaddress(node.getnewaddress(), 1)
        self.wait_until(lambda: locked([baseline]), timeout=30)
        baseline_ms = (time.monotonic() - start) * 1000
        assert_equal(node.gettransaction(baseline)["confirmations"], 0)

        self.bump_mocktime(1)
        self.generate(node, CUTOFF - 1)
        start = time.monotonic()
        # Do not drain the mesh before submitting the transactions: report
        # emission and transaction propagation must compete in the same window.
        self.generate(node, 1, sync_fun=self.no_op)
        txids = [node.sendtoaddress(node.getnewaddress(), 1) for _ in range(4)]
        self.wait_until(lambda: locked(txids), timeout=30)
        burst_ms = (time.monotonic() - start) * 1000
        assert burst_ms <= 30000, f"cutoff InstantSend burst took {burst_ms:.0f} real milliseconds"
        assert_equal(node.getblockcount() % EPOCH_INTERVAL, CUTOFF)
        for txid in txids:
            assert_equal(node.gettransaction(txid)["confirmations"], 0)
        self.sync_blocks()
        def current_reports():
            status = [peer.dslstatus() for peer in self.nodes]
            return all(item["epoch"] == epoch and item["epochreports"] == len(self.mninfo) * (len(self.mninfo) - 1) for item in status)

        self.wait_until(current_reports, timeout=60)
        for peer in self.nodes:
            assert_equal(peer.dslstatus()["missedreports"], 0)
        # Receiving a lock on node1 does not mean node0 has processed it yet.
        # The next template needs the producer's own lock state for these TXs.
        self.wait_until(lambda: all(node.getrawtransaction(txid, True)["instantlock_internal"]
                                   for txid in txids), timeout=30)
        block = self.generate(node, 1)[0]
        self.wait_for_chainlocked_block(receiver, block)
        for txid in txids:
            assert txid in receiver.getblock(block)["tx"]
        self.log.info("Cutoff burst: baseline internal IS %.0f ms, four internal IS locks %.0f ms, new block ChainLocked",
                      baseline_ms, burst_ms)


if __name__ == '__main__':
    DSLPerfBurstTest().main()
