#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""combineoutputs: combine a wallet's small outputs in batches.

A wallet that received many small payments at many addresses holds them as
many outputs, and a staking wallet's own win-time combining never reaches
outputs at other addresses or in wallets that rarely win. combineoutputs
spends them in batches, each one transaction to one new address of the same
wallet, paying its fee out of the combined amount.

What is checked:

- a dry run (the default) reports the batches and their fees and sends
  nothing;
- batches follow batch_size, smallest outputs first, and a lone leftover is
  not sent on its own;
- a real run spends exactly the outputs it reported, all to one new address of
  the wallet, with no change output, and every transaction is in the mempool;
- never taken: a locked output, one at or above max_amount (which on regtest
  is the masternode collateral amount), an unconfirmed one, and anything of
  another wallet;
- an encrypted, locked wallet can dry-run but not send;
- output_size lays each batch out in equal outputs;
- invalid arguments are refused.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# chainparams.cpp, CRegTestParams: regtest has no stakeable floor, so the
# default max_amount is the smaller collateral amount
REGULAR_MN_COLLATERAL = Decimal("1000")
DUST_COUNT = 25


class WalletCombineOutputsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    def fund(self, funder, address, amount):
        txid = funder.sendtoaddress(address, amount)
        tx = funder.getrawtransaction(txid, True)
        for out in tx["vout"]:
            spk = out["scriptPubKey"]
            if out["value"] == amount and (spk.get("address") == address or address in spk.get("addresses", [])):
                return (txid, out["n"])
        raise AssertionError("funding output not found in %s" % txid)

    @staticmethod
    def unspent(wallet, minconf=0):
        return {(u["txid"], u["vout"]): u for u in wallet.listunspent(minconf)}

    def run_test(self):
        node = self.nodes[0]
        funder = node.get_wallet_rpc(self.default_wallet_name)
        mine_to = funder.getnewaddress()
        self.generatetoaddress(node, 120, mine_to)

        node.createwallet(wallet_name="dust", descriptors=self.options.descriptors)
        dust_w = node.get_wallet_rpc("dust")
        # the framework runs with -keypool=1; a legacy wallet hands out no
        # more addresses than its pool holds, and a real run needs one more
        dust_w.keypoolrefill(DUST_COUNT + 10)

        self.log.info("Funding %d small outputs at as many addresses, and one of each kind never taken", DUST_COUNT)
        dust = {}
        for i in range(DUST_COUNT):
            amount = Decimal("3.7") * (i + 1)
            dust[self.fund(funder, dust_w.getnewaddress(), amount)] = amount
        locked = self.fund(funder, dust_w.getnewaddress(), Decimal("5"))
        at_collateral = self.fund(funder, dust_w.getnewaddress(), REGULAR_MN_COLLATERAL)
        above = self.fund(funder, dust_w.getnewaddress(), Decimal("1500"))
        self.generatetoaddress(node, 1, mine_to)
        assert dust_w.lockunspent(False, [{"txid": locked[0], "vout": locked[1]}])
        # stays unconfirmed: minconf 1 must leave it out
        unconfirmed = self.fund(funder, dust_w.getnewaddress(), Decimal("7"))
        never = {locked, at_collateral, above, unconfirmed}
        dust_total = sum(dust.values())

        self.log.info("A dry run reports and sends nothing")
        before = self.unspent(dust_w)
        mempool_before = set(node.getrawmempool())
        res = dust_w.combineoutputs()
        assert_equal(res["dry_run"], True)
        assert_equal(res["candidates"], DUST_COUNT)
        assert_equal(Decimal(str(res["candidate_amount"])), dust_total)
        assert "address" not in res
        assert_equal(len(res["batches"]), 1)
        batch = res["batches"][0]
        assert_equal(batch["inputs"], DUST_COUNT)
        assert "txid" not in batch
        fee = Decimal(str(batch["fee"]))
        assert_greater_than(fee, 0)
        assert_equal([Decimal(str(v)) for v in batch["outputs"]], [dust_total - fee])
        assert_equal(set(node.getrawmempool()), mempool_before)
        assert_equal(self.unspent(dust_w), before)

        self.log.info("batch_size splits the work, smallest first; a lone leftover is not sent")
        res = dust_w.combineoutputs(dry_run=True, batch_size=10)
        assert_equal([b["inputs"] for b in res["batches"]], [10, 10, 5])
        smallest_ten = sorted(dust.values())[:10]
        assert_equal(Decimal(str(res["batches"][0]["amount_in"])), sum(smallest_ten))
        res = dust_w.combineoutputs(dry_run=True, batch_size=12)
        # 25 = 12 + 12 + 1: the last one has nothing to be combined with
        assert_equal([b["inputs"] for b in res["batches"]], [12, 12])

        self.log.info("Invalid arguments are refused")
        assert_raises_rpc_error(-8, "max_amount", dust_w.combineoutputs, dry_run=True, max_amount=REGULAR_MN_COLLATERAL + 1)
        assert_raises_rpc_error(-8, "batch_size", dust_w.combineoutputs, dry_run=True, batch_size=1)
        assert_raises_rpc_error(-8, "batch_size", dust_w.combineoutputs, dry_run=True, batch_size=601)
        assert_raises_rpc_error(-8, "min_amount", dust_w.combineoutputs, dry_run=True, min_amount=10, max_amount=10)

        self.log.info("A real run spends exactly the reported outputs, to one new address, without change")
        res = dust_w.combineoutputs(dry_run=False, batch_size=10)
        assert_equal(res["dry_run"], False)
        address = res["address"]
        assert dust_w.getaddressinfo(address)["ismine"]
        assert_equal(len(res["batches"]), 3)
        spent = set()
        mempool = set(node.getrawmempool())
        for b in res["batches"]:
            assert "error" not in b, b
            assert b["txid"] in mempool
            tx = node.getrawtransaction(b["txid"], True)
            ins = {(v["txid"], v["vout"]) for v in tx["vin"]}
            assert_equal(len(ins), b["inputs"])
            assert ins <= set(dust), "a batch spent an output it was not given"
            assert not (ins & never)
            spent |= ins
            # one output, to the new address, and nothing else: no change
            assert_equal(len(tx["vout"]), 1)
            spk = tx["vout"][0]["scriptPubKey"]
            assert spk.get("address") == address or address in spk.get("addresses", [])
            value_in = sum(dust[o] for o in ins)
            assert_equal(value_in - tx["vout"][0]["value"], Decimal(str(b["fee"])))
        # 25 in batches of 10: 10 + 10 + 5, all of them
        assert_equal(spent, set(dust))
        after = self.unspent(dust_w)
        for op in never:
            assert op in after or op == locked, op
        assert locked in {(u["txid"], u["vout"]) for u in dust_w.listlockunspent()}
        self.generatetoaddress(node, 1, mine_to)
        for b in res["batches"]:
            assert_equal(dust_w.gettransaction(b["txid"])["confirmations"], 1)

        self.log.info("Nothing left below the limit: nothing to do")
        res = dust_w.combineoutputs(dry_run=True, max_amount=Decimal("8"))
        # the unconfirmed 7 is now confirmed and alone below 8
        assert_equal(res["candidates"], 1)
        assert_equal(res["batches"], [])

        self.log.info("minconf applies to the wallet's own unconfirmed outputs")
        # A payment from another wallet is not spendable before it confirms,
        # whatever minconf says; the wallet's own change is. So the rule is
        # exercised by a send to itself, which leaves two unconfirmed outputs
        # the wallet trusts: the payment and the change.
        node.createwallet(wallet_name="conf", descriptors=self.options.descriptors)
        conf_w = node.get_wallet_rpc("conf")
        conf_w.keypoolrefill(10)
        for _ in range(3):
            self.fund(funder, conf_w.getnewaddress(), Decimal("20"))
        self.generatetoaddress(node, 1, mine_to)
        conf_w.sendtoaddress(conf_w.getnewaddress(), Decimal("5"))
        assert_equal(len(conf_w.listunspent(0, 0)), 2)
        # the default, minconf 1: only the two confirmed outputs left
        assert_equal(conf_w.combineoutputs()["candidates"], 2)
        # minconf 0 takes the two unconfirmed ones as well
        assert_equal(conf_w.combineoutputs(dry_run=True, minconf=0)["candidates"], 4)

        self.log.info("output_size lays a batch out in equal outputs")
        node.createwallet(wallet_name="pieces", descriptors=self.options.descriptors)
        pieces_w = node.get_wallet_rpc("pieces")
        pieces_w.keypoolrefill(20)
        for _ in range(10):
            self.fund(funder, pieces_w.getnewaddress(), Decimal("90"))
        self.generatetoaddress(node, 1, mine_to)
        res = pieces_w.combineoutputs(dry_run=False, output_size=Decimal("300"))
        assert_equal(len(res["batches"]), 1)
        tx = node.getrawtransaction(res["batches"][0]["txid"], True)
        values = [o["value"] for o in tx["vout"]]
        assert_equal(len(values), 3)
        assert_equal(sum(values) + Decimal(str(res["batches"][0]["fee"])), Decimal("900"))
        # equal but for the rounding of the fee split
        assert max(values) - min(values) < Decimal("0.00001")

        self.log.info("An encrypted, locked wallet can dry-run but not send")
        node.createwallet(wallet_name="enc", descriptors=self.options.descriptors, passphrase="pass")
        enc_w = node.get_wallet_rpc("enc")
        # an encrypted legacy wallet cannot grow its key pool while locked
        enc_w.walletpassphrase("pass", 60)
        enc_w.keypoolrefill(10)
        for _ in range(3):
            self.fund(funder, enc_w.getnewaddress(), Decimal("4"))
        enc_w.walletlock()
        self.generatetoaddress(node, 1, mine_to)
        res = enc_w.combineoutputs()
        assert_equal(res["candidates"], 3)
        assert_equal(res["batches"][0]["inputs"], 3)
        assert_raises_rpc_error(-13, "passphrase", enc_w.combineoutputs, dry_run=False)
        # nothing was sent by the refused call
        assert_equal(len(self.unspent(enc_w, 1)), 3)


if __name__ == "__main__":
    WalletCombineOutputsTest().main()
