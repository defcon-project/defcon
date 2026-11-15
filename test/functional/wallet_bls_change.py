#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The change of a payment to a BLS address, and of other long output scripts.

A wallet that paid a BLS address sent its change to a new BLS key of its own. A descriptor wallet stores such a key
but does not count what is paid to it as its own, so the change dropped out of its balance: nothing showed it and
nothing could spend it. And the wallet told a BLS payment by the size of the output script alone, so any output of 50
bytes or more, such as a data carrier, sent the change to a new BLS key as well, in either kind of wallet.

What is checked:

- a descriptor wallet that pays a BLS address keeps its change: its balance falls by the amount and the fee, the
  change pays an ordinary address of its own, and no BLS key is added;
- a legacy wallet still sends that change to a new BLS key, which it lists and counts;
- a transaction with a 48-byte data carrier output, one that ends in the byte of OP_CHECKSIG among them, takes ordinary
  change in both kinds of wallet and adds no BLS key.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Data carrier payloads whose output script is 50 bytes long, the length of a BLS one: OP_RETURN, a push of 48, and 48
# bytes. The second ends in 0xac, OP_CHECKSIG, as a BLS script does.
PAYLOADS = ["ab" * 48, "ab" * 47 + "ac"]


class WalletBLSChangeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()
        self.skip_if_no_sqlite()

    def confirm(self, mine_to):
        self.generatetoaddress(self.nodes[0], 1, mine_to)
        self.nodes[0].syncwithvalidationinterfacequeue()

    def run_test(self):
        node = self.nodes[0]
        funder = node.get_wallet_rpc(self.default_wallet_name)
        mine_to = funder.getnewaddress()
        self.generatetoaddress(node, 120, mine_to)

        self.log.info("The BLS address paid: a key of a third, legacy wallet")
        node.createwallet(wallet_name="payee", descriptors=False)
        payee = node.get_wallet_rpc("payee")
        payee.bls("generate")
        bls_address = payee.listblsaddresses()["0"]
        bls_script = payee.getaddressinfo(bls_address)["scriptPubKey"]
        assert_equal(len(bls_script), 100)

        wallets = {}
        for kind, descriptors in (("descriptor", True), ("legacy", False)):
            node.createwallet(wallet_name=kind, descriptors=descriptors)
            wallet = node.get_wallet_rpc(kind)
            funder.sendtoaddress(wallet.getnewaddress(), Decimal("100"))
            wallets[kind] = wallet
        self.confirm(mine_to)
        for wallet in wallets.values():
            assert_equal(wallet.getbalance(), Decimal("100"))

        for kind, wallet in wallets.items():
            self.log.info(f"A {kind} wallet pays a BLS address and keeps its change")
            keys_before = len(wallet.listblsaddresses())
            before = wallet.getbalance()
            txid = wallet.sendtoaddress(bls_address, Decimal("10"))
            fee = -wallet.gettransaction(txid)["fee"]
            self.confirm(mine_to)
            assert_equal(wallet.getbalance(), before - Decimal("10") - fee)
            tx = wallet.decoderawtransaction(wallet.gettransaction(txid)["hex"])
            change = [o for o in tx["vout"] if o["scriptPubKey"]["hex"] != bls_script]
            assert_equal(len(change), 1)
            change_type = change[0]["scriptPubKey"]["type"]
            if kind == "descriptor":
                assert_equal(change_type, "pubkeyhash")
                assert wallet.getaddressinfo(change[0]["scriptPubKey"]["address"])["ismine"]
                assert_equal(len(wallet.listblsaddresses()), keys_before)
            else:
                # Unchanged: the legacy wallet's change goes to a new BLS key, which it knows.
                assert_equal(change_type, "blspubkey")
                assert_equal(len(wallet.listblsaddresses()), keys_before + 1)
            assert_equal(sum(u["amount"] for u in wallet.listunspent()), wallet.getbalance())

        for kind, wallet in wallets.items():
            for payload in PAYLOADS:
                self.log.info(f"A {kind} wallet funds a 50-byte data carrier output ({payload[-2:]} last) with ordinary change")
                keys_before = len(wallet.listblsaddresses())
                raw = wallet.createrawtransaction([], [{"data": payload}, {funder.getnewaddress(): Decimal("1")}])
                funded = wallet.fundrawtransaction(raw)
                assert funded["changepos"] >= 0
                tx = wallet.decoderawtransaction(funded["hex"])
                carrier = [o for o in tx["vout"] if o["scriptPubKey"]["type"] == "nulldata"]
                assert_equal(len(carrier), 1)
                assert_equal(len(carrier[0]["scriptPubKey"]["hex"]), 100)
                change = tx["vout"][funded["changepos"]]["scriptPubKey"]
                assert_equal(change["type"], "pubkeyhash")
                assert wallet.getaddressinfo(change["address"])["ismine"]
                assert_equal(len(wallet.listblsaddresses()), keys_before)


if __name__ == '__main__':
    WalletBLSChangeTest().main()
