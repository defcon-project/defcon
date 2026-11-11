#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A locked wallet knows the payments to its own BLS addresses.

An encrypted wallet used to know its BLS keys, public keys and all, only while it was fully unlocked: it opened
without them, and locking dropped them. A payment to one of its BLS addresses that arrived while the wallet was
locked, or unlocked for mixing and staking only, was not recorded, and stayed missing until a rescan; payments it
had recorded dropped out of the balance while it was locked.

The public key is stored in the clear next to the encrypted secret, so the wallet now knows it from the moment it
opens, and locking takes away the secret alone.

What is checked, on an encrypted legacy wallet (the kind whose ownership check looks at BLS keys):

- a payment to its BLS address is recorded when it arrives and counts in the balance once confirmed, while the
  wallet is locked and while it is unlocked for mixing and staking only;
- locked, the wallet lists its BLS address and reports it as its own;
- after a restart the locked wallet shows the same balance without a rescan, and records a new payment;
- spending needs the full unlock: sending and signing are refused before it, and work after it.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

PASSPHRASE = "bls locked wallet passphrase"
UNLOCK_NEEDED = "Please enter the wallet passphrase with walletpassphrase first"


class WalletBLSLockedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def pay(self, funder, address, amount):
        txid = funder.sendtoaddress(address, amount)
        # The other wallet hears of the payment through the validation queue, not by the time sendtoaddress returns.
        self.nodes[0].syncwithvalidationinterfacequeue()
        return txid

    def confirm(self, mine_to):
        self.generatetoaddress(self.nodes[0], 1, mine_to)
        self.nodes[0].syncwithvalidationinterfacequeue()

    def run_test(self):
        node = self.nodes[0]
        funder = node.get_wallet_rpc(self.default_wallet_name)
        mine_to = funder.getnewaddress()
        self.generatetoaddress(node, 120, mine_to)

        self.log.info("An encrypted wallet with one BLS key, locked")
        node.createwallet(wallet_name="holder", descriptors=False, passphrase=PASSPHRASE, load_on_startup=True)
        holder = node.get_wallet_rpc("holder")
        holder.walletpassphrase(PASSPHRASE, 600)
        holder.bls("generate")
        addresses = holder.listblsaddresses()
        assert_equal(len(addresses), 1)
        address = addresses["0"]
        script = holder.getaddressinfo(address)["scriptPubKey"]
        holder.walletlock()

        self.log.info("A payment that arrives while the wallet is locked is recorded, and counts once confirmed")
        first = self.pay(funder, address, Decimal("7"))
        assert_equal(holder.gettransaction(first)["amount"], Decimal("7"))
        assert_equal(holder.getbalances()["mine"]["untrusted_pending"], Decimal("7"))
        self.confirm(mine_to)
        assert_equal(holder.getbalance(), Decimal("7"))

        self.log.info("Locked, the wallet lists its BLS address and reports it as its own")
        assert_equal(holder.listblsaddresses(), addresses)
        assert_equal(holder.getaddressinfo(address)["ismine"], True)

        self.log.info("The same while it is unlocked for mixing and staking only, and that unlock does not let it spend")
        holder.walletpassphrase(PASSPHRASE, 600, True)
        second = self.pay(funder, address, Decimal("3"))
        assert_equal(holder.gettransaction(second)["amount"], Decimal("3"))
        self.confirm(mine_to)
        assert_equal(holder.getbalance(), Decimal("10"))
        assert_raises_rpc_error(-13, UNLOCK_NEEDED, holder.sendtoaddress, mine_to, 1)
        holder.walletlock()
        assert_equal(holder.getbalance(), Decimal("10"))

        self.log.info("After a restart the locked wallet shows the same without a rescan, and records a new payment")
        self.restart_node(0)
        for name in (self.default_wallet_name, "holder"):
            if name not in node.listwallets():
                node.loadwallet(name)
        funder = node.get_wallet_rpc(self.default_wallet_name)
        holder = node.get_wallet_rpc("holder")
        assert_equal(holder.getwalletinfo()["unlocked_until"], 0)
        assert_equal(holder.listblsaddresses(), addresses)
        assert_equal(holder.getbalance(), Decimal("10"))
        third = self.pay(funder, address, Decimal("2"))
        assert_equal(holder.gettransaction(third)["amount"], Decimal("2"))
        self.confirm(mine_to)
        assert_equal(holder.getbalance(), Decimal("12"))
        unspent = {(u["txid"], u["vout"]): u for u in holder.listunspent()}
        assert_equal(sorted((u["txid"], u["amount"], u["spendable"], u["scriptPubKey"]) for u in unspent.values()),
                     sorted((txid, amount, True, script) for txid, amount in ((first, Decimal("7")), (second, Decimal("3")), (third, Decimal("2")))))

        self.log.info("Spending them needs the full unlock")
        assert_raises_rpc_error(-13, UNLOCK_NEEDED, holder.sendtoaddress, mine_to, 1)
        (txid, vout) = next(outpoint for outpoint, u in unspent.items() if u["amount"] == Decimal("7"))
        raw = node.createrawtransaction([{"txid": txid, "vout": vout}], {mine_to: Decimal("6.99")})
        assert_raises_rpc_error(-13, UNLOCK_NEEDED, holder.signrawtransactionwithwallet, raw)
        holder.walletpassphrase(PASSPHRASE, 600)
        signed = holder.signrawtransactionwithwallet(raw)
        assert_equal(signed["complete"], True)
        spend = node.sendrawtransaction(signed["hex"])
        assert spend in node.getrawmempool()
        node.syncwithvalidationinterfacequeue()
        assert_equal(holder.getbalances()["mine"]["trusted"], Decimal("5"))
        holder.walletlock()


if __name__ == '__main__':
    WalletBLSLockedTest().main()
