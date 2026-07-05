#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test encrypted wallet handling for stored BLS private keys."""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class WalletBLSEncryptionTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def bls_generate(self):
        return getattr(self.nodes[0], "bls generate")()

    def wallet_file(self):
        wallets_dir = os.path.join(self.nodes[0].datadir, self.chain, "wallets")
        for root, _, files in os.walk(wallets_dir):
            if self.wallet_data_filename in files:
                return os.path.join(root, self.wallet_data_filename)
        raise AssertionError(f"Could not find {self.wallet_data_filename} below {wallets_dir}")

    def assert_wallet_file_contains(self, needle, expected):
        with open(self.wallet_file(), "rb") as wallet:
            found = needle.encode() in wallet.read()
        assert_equal(found, expected)

    def run_test(self):
        passphrase = "bls wallet passphrase"

        self.log.info("Generate a BLS key in an unencrypted wallet")
        first_key = self.bls_generate()
        first_secret = first_key["secret"]
        assert_equal(len(self.nodes[0].listblsaddresses()), 1)

        self.log.info("Verify the legacy unencrypted wallet stored the BLS secret")
        self.stop_node(0)
        self.assert_wallet_file_contains(first_secret, True)

        self.log.info("Encrypt the wallet and migrate the legacy BLS key")
        self.start_node(0)
        self.nodes[0].encryptwallet(passphrase)

        self.log.info("Locked encrypted wallet must not expose or generate BLS private keys")
        assert_equal(self.nodes[0].listblsaddresses(), {})
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", self.bls_generate)

        self.log.info("Verify the migrated BLS secret is no longer present in wallet.dat")
        self.stop_node(0)
        self.assert_wallet_file_contains(first_secret, False)

        self.log.info("Encrypted BLS keys remain unavailable after restart until wallet unlock")
        self.start_node(0)
        assert_equal(self.nodes[0].listblsaddresses(), {})
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", self.bls_generate)

        self.log.info("Unlock loads encrypted BLS keys and allows new encrypted BLS key generation")
        self.nodes[0].walletpassphrase(passphrase, 600)
        assert_equal(len(self.nodes[0].listblsaddresses()), 1)
        second_key = self.bls_generate()
        second_secret = second_key["secret"]
        assert_equal(len(self.nodes[0].listblsaddresses()), 2)

        self.log.info("Lock clears plaintext BLS keys from memory")
        self.nodes[0].walletlock()
        assert_equal(self.nodes[0].listblsaddresses(), {})
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", self.bls_generate)

        self.log.info("Both encrypted BLS keys reload after unlock and are absent from wallet.dat")
        self.nodes[0].walletpassphrase(passphrase, 600)
        assert_equal(len(self.nodes[0].listblsaddresses()), 2)
        self.stop_node(0)
        self.assert_wallet_file_contains(first_secret, False)
        self.assert_wallet_file_contains(second_secret, False)


if __name__ == "__main__":
    WalletBLSEncryptionTest().main()
