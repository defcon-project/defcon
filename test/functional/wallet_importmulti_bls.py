#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""importmulti must name a BLS script for what it is.

The switch that classifies a script for import had no case for BLS outputs, so
one reached unreachable-code handling instead. The throw that raised was caught
by the catch-all around each request and reported as "Missing required fields"
-- an answer about a different problem than the one the caller had.

Every other type this path cannot solve is imported watch-only with a warning
that says why, and a BLS script now joins them: importblsprivkey is the path
that can hold such a key.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# An arbitrary but valid 32-byte BLS scalar.
BLS_PRIVKEY = "2b7c1f5e9a04d38c6be0f1472a95d83e6c11ba7d4f2e08c95a3d716be4c0928f"


class WalletImportMultiBLSTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Build a BLS script by importing a key into a wallet of its own")
        node.createwallet(wallet_name="holder", descriptors=False)
        holder = node.get_wallet_rpc("holder")
        holder.importblsprivkey(BLS_PRIVKEY, False)
        addresses = holder.listblsaddresses()
        assert len(addresses) > 0, "importblsprivkey stored nothing"
        bls_script = holder.getaddressinfo(list(addresses.values())[0])["scriptPubKey"]

        node.createwallet(wallet_name="importer", descriptors=False)
        importer = node.get_wallet_rpc("importer")
        # A key has to accompany the script or the request carries no solving data
        # and the classifying switch is never consulted at all. Which key is
        # irrelevant -- it is the script that decides the answer -- so take one the
        # importing wallet does not hold, which is what a watch-only import means.
        pubkey = holder.getaddressinfo(holder.getnewaddress())["pubkey"]

        self.log.info("importmulti must explain a BLS script instead of misreporting it")
        result = importer.importmulti([{
            "scriptPubKey": bls_script,
            "pubkeys": [pubkey],
            "timestamp": "now",
            "watchonly": True,
        }])

        assert_equal(len(result), 1)
        # Unsolvable-but-importable is the same outcome every other unsupported
        # type gets here; the reason belongs in the warning, not in a failure.
        assert_equal(result[0]["success"], True)
        warnings = result[0].get("warnings", [])
        assert any("importblsprivkey" in w for w in warnings), warnings

        self.log.info("Passed")


if __name__ == '__main__':
    WalletImportMultiBLSTest().main()
