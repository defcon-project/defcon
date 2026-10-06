#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see COPYING.
"""Seven wallets: staking info stays correctly attributed during registry rebuilds."""
from concurrent.futures import ThreadPoolExecutor
from threading import Event

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy


class StakingInfoMultiwalletTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-staking=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    def run_test(self):
        node = self.nodes[0]
        funder = node.get_wallet_rpc(self.default_wallet_name)
        mine_to = funder.getnewaddress()
        self.generatetoaddress(node, 110, mine_to)
        expected = {}
        for i in range(7):
            name = "combine-info-%d" % i
            node.createwallet(wallet_name=name, descriptors=self.options.descriptors)
            wallet = node.get_wallet_rpc(name)
            address = wallet.getnewaddress()
            for _ in range(i + 1):
                funder.sendtoaddress(address, 5)
            expected[name] = i + 1
        self.generatetoaddress(node, 1, mine_to)

        def names():
            return {v["name"] for v in node.getstakinginfo().values()}

        self.wait_until(lambda: set(expected) <= names())
        stop = Event()

        def query():
            rpc = get_rpc_proxy(node.url, 0, timeout=60, coveragedir=node.coverage_dir)
            calls = 0
            while not stop.is_set():
                info = rpc.getstakinginfo()
                seen = [v["name"] for v in info.values()]
                assert_equal(len(seen), len(set(seen)))
                for row in info.values():
                    if row["name"] in expected:
                        assert_equal(row["stake_outputs"], expected[row["name"]])
                        assert_equal(row["stake_combine"], True)
                calls += 1
                stop.wait(0.02)
            return calls

        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(query)
            try:
                for name in list(expected)[:4]:
                    node.unloadwallet(name)
                    self.wait_until(lambda: name not in names())
                    node.loadwallet(name)
                    self.wait_until(lambda: name in names())
            finally:
                stop.set()
            assert future.result() > 10
        assert set(expected) <= names()


if __name__ == "__main__":
    StakingInfoMultiwalletTest().main()
