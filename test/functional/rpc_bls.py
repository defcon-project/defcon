#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BLS key generation with and without compiled wallet support."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class BLSRPCTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        if self.is_wallet_compiled():
            if self.options.descriptors:
                self.skip_if_no_sqlite()
            else:
                self.skip_if_no_bdb()

    def run_test(self):
        node = self.nodes[0]
        # These registered RPCs keep the same empty-registry contract in both builds.
        for command in ("getstakinginfo", "liststakingwallets", "setstaking"):
            assert command in node.help(command)
        assert_equal(node.getstakinginfo(), {})
        assert_equal(node.liststakingwallets(), {})
        for wallet_id in (-1, 0, 1, None):
            assert_equal(node.setstaking(wallet_id), None)
        assert_raises_rpc_error(-1, "JSON value is not an integer", node.setstaking, "invalid")
        for wallet_id in (1.5, 2**31):
            assert_raises_rpc_error(-1, "JSON integer out of range", node.setstaking, wallet_id)
        assert_raises_rpc_error(-1, "setstaking", node.setstaking)

        if self.is_wallet_compiled():
            node.createwallet("bls", descriptors=self.options.descriptors, load_on_startup=True)
            self.wait_until(lambda: any(wallet["name"] == "bls" for wallet in node.liststakingwallets().values()))
            assert_equal(node.getstakinginfo()["0"]["name"], "bls")
            assert_equal(node.setstaking(0), True)
            assert_equal(node.setstaking(0), False)

        assert "generate" in node.help("bls")
        assert "BLS secret/public key pair" in node.help("bls generate")
        for args, scheme in [((), "basic"), ((False,), "basic"), ((True,), "legacy")]:
            key = node.bls("generate", *args)
            assert_equal(set(key), {"secret", "public", "scheme"})
            assert_equal(len(key["secret"]), 64)
            assert_equal(len(key["public"]), 96)
            assert_equal(key["scheme"], scheme)
            assert_equal(node.bls("fromsecret", key["secret"], scheme == "legacy"), key)

        assert_raises_rpc_error(-8, "bls_legacy_scheme must be", node.bls, "generate", "invalid")
        if self.is_wallet_compiled():
            assert_equal(len(node.listblsaddresses()), 3)
            self.restart_node(0)
            assert_equal(len(node.listblsaddresses()), 3)


if __name__ == "__main__":
    BLSRPCTest().main()
