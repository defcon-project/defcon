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
  change in both kinds of wallet and adds no BLS key;
- a legacy wallet made without private keys makes no BLS key for its change: a payment that leaves change is refused
  until a change address is named, and one that leaves none is funded without one.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# Data carrier payloads whose output script is 50 bytes long, the length of a BLS one: OP_RETURN, a push of 48, and 48
# bytes. The second ends in 0xac, OP_CHECKSIG, as a BLS script does.
PAYLOADS = ["ab" * 48, "ab" * 47 + "ac"]


class WalletBLSChangeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The default, named: the last case's amounts are worked out from it.
        self.extra_args = [["-discardfee=0.0001"]]

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

        self.log.info("A legacy wallet without private keys makes no BLS key for its change")
        node.createwallet(wallet_name="nokeys", descriptors=False, disable_private_keys=True)
        nokeys = node.get_wallet_rpc("nokeys")
        watched = funder.getnewaddress()
        nokeys.importpubkey(funder.getaddressinfo(watched)["pubkey"])
        funder.sendtoaddress(watched, Decimal("100"))
        self.confirm(mine_to)
        assert_equal(nokeys.getbalance(), Decimal("100"))
        outputs = [{bls_address: Decimal("10")}]
        assert_raises_rpc_error(-4, "Transaction needs a BLS change address, which a wallet without private keys cannot make. Choose a change address.",
                                nokeys.walletcreatefundedpsbt, [], outputs)
        assert_equal(nokeys.listblsaddresses(), {})
        # With a change address named, the same payment is funded, and still no BLS key is made.
        funded = nokeys.walletcreatefundedpsbt([], outputs, 0, {"changeAddress": watched})
        assert funded["changepos"] >= 0
        tx = nokeys.decodepsbt(funded["psbt"])["tx"]
        assert_equal(tx["vout"][funded["changepos"]]["scriptPubKey"]["address"], watched)
        assert_equal(nokeys.listblsaddresses(), {})
        # A payment that leaves no change needs no change address: the whole coin, the fee taken from the amount.
        whole = nokeys.walletcreatefundedpsbt([], [{bls_address: Decimal("100")}], 0, {"subtractFeeFromOutputs": [0]})
        assert_equal(whole["changepos"], -1)
        tx = nokeys.decodepsbt(whole["psbt"])["tx"]
        assert_equal(len(tx["vout"]), 1)
        assert_equal(tx["vout"][0]["scriptPubKey"]["hex"], bls_script)
        assert_equal(tx["vout"][0]["value"], Decimal("100") - whole["fee"])
        assert_equal(nokeys.listblsaddresses(), {})
        # A payment that leaves too little for a change output: what counts as too little is measured with a change
        # output of a BLS address's size, as in a wallet that can make one, and the rest goes to the fee. At 1 duff
        # per byte and the default discard rate (10 duffs per byte) 1850 duffs are left: a change output to a BLS
        # address (59 bytes) would hold 1791, under the 2070 it has to hold; measured with no script (9 bytes) it
        # would hold 1841, over 1570, and the payment would be refused for the change it does not need.
        same_shape = nokeys.walletcreatefundedpsbt([], [{bls_address: Decimal("100")}], 0, {"subtractFeeFromOutputs": [0], "fee_rate": 1})
        left = Decimal("0.00001850")
        edge = nokeys.walletcreatefundedpsbt([], [{bls_address: Decimal("100") - same_shape["fee"] - left}], 0, {"fee_rate": 1})
        assert_equal(edge["changepos"], -1)
        assert_equal(edge["fee"], same_shape["fee"] + left)
        assert_equal(len(nokeys.decodepsbt(edge["psbt"])["tx"]["vout"]), 1)
        assert_equal(nokeys.listblsaddresses(), {})
        # Nor did any of these payments leave a BLS key in the wallet's file: loaded again, it lists none.
        node.unloadwallet("nokeys")
        node.loadwallet("nokeys")
        assert_equal(nokeys.listblsaddresses(), {})


if __name__ == '__main__':
    WalletBLSChangeTest().main()
