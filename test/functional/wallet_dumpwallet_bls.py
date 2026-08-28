#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""dumpwallet must not present itself as a complete backup when it is not.

BLS private keys are kept in their own wallet records and never enter the key
metadata map that dumpwallet walks, so they are absent from the dumpfile and
importwallet cannot bring them back. The command used to describe its output as
"all private keys from this wallet", which is the one claim that turns a missing
key into a lost key: someone trusts the dump, discards the wallet file, and finds
out later.

This test pins the disclosure, not a format: a wallet holding a BLS key must say
so in the reply and in the dumpfile itself, and the dump must still not contain
the key.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

# An arbitrary but valid 32-byte BLS scalar; the value is irrelevant to the test.
BLS_PRIVKEY = "2b7c1f5e9a04d38c6be0f1472a95d83e6c11ba7d4f2e08c95a3d716be4c0928f"


class DumpWalletBLSTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("A wallet with no BLS keys keeps the plain reply")
        node.createwallet(wallet_name="plain", descriptors=False)
        plain = node.get_wallet_rpc("plain")
        plain_dump = os.path.join(self.nodes[0].datadir, "plain.dump")
        result = plain.dumpwallet(plain_dump)
        assert "bls_keys_not_dumped" not in result
        assert "BLS" not in result["warning"]

        self.log.info("A wallet holding a BLS key must disclose that the dump omits it")
        node.createwallet(wallet_name="withbls", descriptors=False)
        wallet = node.get_wallet_rpc("withbls")
        wallet.importblsprivkey(BLS_PRIVKEY, False)
        assert_greater_than(len(wallet.listblsaddresses()), 0)

        dump_path = os.path.join(self.nodes[0].datadir, "withbls.dump")
        result = wallet.dumpwallet(dump_path)

        # The reply has to carry it, both for a human and for a backup script.
        assert_equal(result["bls_keys_not_dumped"], 1)
        assert "BLS" in result["warning"], result["warning"]

        with open(dump_path, encoding="utf8") as f:
            dump = f.read()

        # The file has to carry it too: it outlives the RPC call, and it is what
        # someone actually restores from.
        assert "BLS private key" in dump, "the dumpfile does not disclose the omission"

        # And the key genuinely is not there -- the disclosure must be true.
        assert BLS_PRIVKEY not in dump
        assert BLS_PRIVKEY.upper() not in dump

        self.log.info("Passed")


if __name__ == '__main__':
    DumpWalletBLSTest().main()
