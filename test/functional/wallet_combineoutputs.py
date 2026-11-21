#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""combineoutputs: combine a wallet's small outputs in batches.

A wallet that received many small payments at many addresses holds them as
many outputs. A staking wallet's own combining takes at most 19 of them, and
only when it wins (with -stakecombinescope=key, only the winning key's).
combineoutputs spends them in batches, each one transaction to one new address
of the same wallet, paying its fee out of the combined amount.

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
- an encrypted, locked wallet can dry-run but not send, and its previews take
  no key, with a partly used key pool and (legacy) an empty one;
- output_size lays each batch out in equal outputs;
- invalid arguments are refused;
- CJ4-B: a batch with one, two or three pieces where any piece is exactly
  10 000.1 DFCN, a CoinJoin denomination that staking skips, fails as not
  stakeable in the preview and in the send alike (one piece: staking_only;
  more: output_size), and a batch of as many pieces where every output is
  10 000.1 DFCN and a duff is stakeable; two 100.001 DFCN coins are no
  candidates. With descriptors, the preview sizes each batch for the script
  the send pays to: pkh(), sh(pkh()) and pk() receiving descriptors (pk()
  pays to P2PKH), and P2PKH with none active, where the send fails at its
  address request.
"""

from decimal import Decimal, ROUND_CEILING

from test_framework.descriptors import descsum_create
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
# the largest DFCN-scale CoinJoin denomination: staking skips a coin of exactly this amount
TOP_DENOM = Decimal("10000.1")
DUFF = Decimal("0.00000001")
STAKE_ERROR = "Batch cannot be laid out within staking amount limits"
# inputs per batch for one, two and three pieces, each input below max_amount
INPUTS = {1: 11, 2: 21, 3: 31}
FEE_RATE = Decimal("0.0001")  # per kB: ten duffs a byte, so two bytes of script show in the fee
XPRV = "tprv8ZgxMBicQKsPcwuZGKp8TeWppSuLMiLe2d9PupB14QpPeQsqoj3LneJLhGHH13xESfvASyd4EFLJvLrG8b7DrLxEuV7hpF9uUc6XruKA1Wq"


def assert_refused(batch, where):
    """The batch failed as not stakeable; where (preview or send) names the call in a failure."""
    assert batch.get("error") == STAKE_ERROR, "%s not refused: %s" % (where, batch)


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

    @staticmethod
    def lock_at(wallet, address):
        coins = wallet.listunspent(1, 9999999, [address])
        assert_equal(len(coins), 1)
        assert wallet.lockunspent(False, [{"txid": coins[0]["txid"], "vout": coins[0]["vout"]}])

    def exact_pieces(self, node, funder, mine_to, w, addresses, k, sends, script_type):
        """One batch of k pieces (k = 1: staking_only; otherwise output_size 10 000.1) from coins in a band of its own.

        The fee depends on the number of inputs and on the script paid to, not on the amounts: a preview of n coins
        gives it, then one of the coins is locked and a coin of the same kind takes its place, so that a piece is
        exactly 10 000.1 while the smallest (two pieces) or the largest (three) output is not (below), and after that
        one so that every piece is 10 000.1 and a duff. sends: whether a send gets an address. Returns the fee and the
        size of the preview.
        """
        n = INPUTS[k]
        band = (k * TOP_DENOM / n).quantize(Decimal("0.01"), rounding=ROUND_CEILING) + Decimal("0.01")
        args = {"min_amount": band - 1, "max_amount": band + 1}
        if k == 1:
            args["staking_only"] = True
        else:
            args["output_size"] = TOP_DENOM
        body = [next(addresses) for _ in range(n)]
        funder.sendmany("", {address: band for address in body})
        self.generatetoaddress(node, 1, mine_to)
        probe = w.combineoutputs(**args)
        assert_equal(probe["candidates"], n)
        assert_equal(len(probe["batches"]), 1)
        batch = probe["batches"][0]
        assert "error" not in batch, batch
        assert_equal(len(batch["outputs"]), k)
        fee = Decimal(str(batch["fee"]))
        size = batch["size"]

        def replace(locked, total):
            self.lock_at(w, locked)
            address = next(addresses)
            amount = total - (n - 1) * band
            assert band - 1 <= amount < band + 1, amount
            funder.sendtoaddress(address, amount)
            self.generatetoaddress(node, 1, mine_to)
            return address

        # The RPC lays out k recipients: the first gets amount_in - each * (k - 1), every other each = amount_in / k;
        # each pays fee / k, the first also the remainder s; the transaction's outputs are then sorted by amount.
        # Two pieces: the smallest output, the first, is 10 000.1 less a duff, so a check of the first output alone
        # passes the batch; with s > 0 the other piece is exactly 10 000.1 (the first ends a duff lower), with s = 0
        # the other piece is a duff lower and the first, the larger, is exactly 10 000.1. Three pieces: three duffs
        # more, so the largest output, the last, is 10 000.1 and a duff and a check of the last output alone passes
        # the batch: [D, D + 1, D + 1] with s > 0, [D, D, D + 1] with s = 0.
        q, s = divmod(int(fee / DUFF), k)
        if k == 1:
            total = TOP_DENOM + fee
        elif s > 0:
            total = k * (TOP_DENOM + q * DUFF) + (s - 1) * DUFF
        else:
            total = k * (TOP_DENOM + (q - 1) * DUFF) + DUFF
        if k == 3:
            total += 3 * DUFF
        exact = replace(body[-1], total)
        self.log.info("%d piece(s) with 10 000.1 DFCN among them: refused in the preview and in the send", k)
        res = w.combineoutputs(**args)
        assert_equal(res["candidates"], n)
        assert_equal(len(res["batches"]), 1)
        assert_refused(res["batches"][0], "preview")
        assert_equal(Decimal(str(res["batches"][0]["amount_in"])), total)
        assert "outputs" not in res["batches"][0]
        if k == 1:
            # without staking_only the piece is laid out and reported as not stakeable
            plain = w.combineoutputs(min_amount=band - 1, max_amount=band + 1)["batches"][0]
            assert "error" not in plain, plain
            assert_equal(plain["stakeable_amounts"], False)
            assert_equal([Decimal(str(v)) for v in plain["outputs"]], [TOP_DENOM])
            assert_equal(Decimal(str(plain["fee"])), fee)
        before = self.unspent(w)
        mempool_before = set(node.getrawmempool())
        if sends:
            res = w.combineoutputs(dry_run=False, **args)
            assert "address" in res
            assert_equal(len(res["batches"]), 1)
            assert_refused(res["batches"][0], "send")
            assert "txid" not in res["batches"][0]
        else:
            assert_raises_rpc_error(-12, "No addresses available", w.combineoutputs, dry_run=False, **args)
        assert_equal(self.unspent(w), before)
        assert_equal(set(node.getrawmempool()), mempool_before)

        replace(exact, k * (TOP_DENOM + DUFF) + fee)
        self.log.info("%d piece(s) of 10 000.1 DFCN and a duff: stakeable", k)
        res = w.combineoutputs(**args)
        batch = res["batches"][0]
        assert "error" not in batch, batch
        assert_equal(batch["stakeable_amounts"], True)
        assert_equal([Decimal(str(v)) for v in batch["outputs"]], [TOP_DENOM + DUFF] * k)
        assert_equal(Decimal(str(batch["fee"])), fee)
        assert_equal(batch["size"], size)
        if not sends:
            assert_raises_rpc_error(-12, "No addresses available", w.combineoutputs, dry_run=False, **args)
            return fee, size
        res = w.combineoutputs(dry_run=False, **args)
        sent = res["batches"][0]
        assert "error" not in sent, sent
        assert_equal(sent["stakeable_amounts"], True)
        assert_equal(Decimal(str(sent["fee"])), fee)
        tx = node.getrawtransaction(sent["txid"], True)
        assert_equal(len(tx["vout"]), k)
        for out in tx["vout"]:
            assert_equal(out["value"], TOP_DENOM + DUFF)
            spk = out["scriptPubKey"]
            assert_equal(spk["type"], script_type)
            assert spk.get("address") == res["address"] or res["address"] in spk.get("addresses", [])
        self.generatetoaddress(node, 1, mine_to)
        return fee, size

    def test_coinjoin_amount_pieces(self, node, funder, mine_to):
        self.log.info("Two confirmed 100.001 DFCN coins are no candidates")
        node.createwallet(wallet_name="two", descriptors=self.options.descriptors)
        two = node.get_wallet_rpc("two")
        two.keypoolrefill(5)
        funder.sendmany("", {two.getnewaddress(): Decimal("100.001"), two.getnewaddress(): Decimal("100.001")})
        self.generatetoaddress(node, 1, mine_to)
        res = two.combineoutputs()
        assert_equal(res["candidates"], 0)
        assert_equal(res["batches"], [])

        if not self.options.descriptors:
            node.createwallet(wallet_name="cj_legacy", descriptors=False)
            w = node.get_wallet_rpc("cj_legacy")
            w.keypoolrefill(80)
            w.settxfee(FEE_RATE)
            addresses = iter([w.getnewaddress() for _ in range(70)])
            for k in (1, 2, 3):
                self.exact_pieces(node, funder, mine_to, w, addresses, k, True, "pubkeyhash")
            return

        # Every wallet holds P2PKH coins of an inactive pkh() descriptor, so the inputs are alike and only the
        # receiving descriptor, which the send pays to, differs.
        kinds = [("pkh", "pkh(%s/21/*)", "pubkeyhash"), ("shpkh", "sh(pkh(%s/22/*))", "scripthash"),
                 ("pk", "pk(%s/23/*)", "pubkeyhash"), ("inactive", None, None)]
        fee = {}
        size = {}
        for path, (name, receiving, script_type) in enumerate(kinds, start=11):
            self.log.info("Exact pieces, receiving descriptor: %s", receiving % "xprv" if receiving else "none active")
            node.createwallet(wallet_name="cj_" + name, blank=True, descriptors=True)
            w = node.get_wallet_rpc("cj_" + name)
            funding = descsum_create("pkh(%s/%d/*)" % (XPRV, path))
            requests = [{"desc": funding, "timestamp": "now", "active": False, "range": [0, 99]}]
            if receiving:
                requests.append({"desc": descsum_create(receiving % XPRV), "timestamp": "now", "active": True, "range": [0, 9]})
            for result in w.importdescriptors(requests):
                assert result["success"], result
            w.settxfee(FEE_RATE)
            addresses = iter(node.deriveaddresses(funding, [0, 99]))
            for k in (1, 2, 3):
                fee[name, k], size[name, k] = self.exact_pieces(node, funder, mine_to, w, addresses, k, receiving is not None, script_type)
        for k in (1, 2, 3):
            # pk() pays to P2PKH; with no active receiving descriptor the preview sizes as before, P2PKH
            assert_equal(size["pk", k], size["pkh", k])
            assert_equal(size["inactive", k], size["pkh", k])
            # a P2SH output script is two bytes shorter than a P2PKH one
            assert_equal(size["pkh", k] - size["shpkh", k], 2 * k)
            assert_greater_than(fee["pkh", k], fee["shpkh", k])

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
        # The dust wallet learns of a payment from another wallet through the validation queue, not by the
        # time sendtoaddress returns; wait for it, so that every snapshot below sees the same outputs.
        node.syncwithvalidationinterfacequeue()
        assert unconfirmed in self.unspent(dust_w)
        never = {locked, at_collateral, above, unconfirmed}
        dust_total = sum(dust.values())

        self.log.info("A dry run reports and sends nothing")
        before = self.unspent(dust_w)
        mempool_before = set(node.getrawmempool())
        keypool_before = dust_w.getwalletinfo()
        descriptors_before = dust_w.listdescriptors() if self.options.descriptors else None
        for _ in range(3):
            res = dust_w.combineoutputs()
        keypool_after = dust_w.getwalletinfo()
        for field in ("keypoolsize", "keypoolsize_hd_internal"):
            assert_equal(keypool_before.get(field), keypool_after.get(field))
        if descriptors_before is not None:
            assert_equal(descriptors_before, dust_w.listdescriptors())
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

        self.log.info("An impossible output count is refused before allocating recipients")
        tiny = dust_w.combineoutputs(output_size=Decimal("0.00000001"))
        assert "standard transaction size" in tiny["batches"][0]["error"]
        assert_equal(self.unspent(dust_w), before)

        self.log.info("Invalid arguments are refused")
        assert_raises_rpc_error(-8, "max_amount", dust_w.combineoutputs, dry_run=True, max_amount=REGULAR_MN_COLLATERAL + 1)
        assert_raises_rpc_error(-8, "batch_size", dust_w.combineoutputs, dry_run=True, batch_size=1)
        assert_raises_rpc_error(-8, "batch_size", dust_w.combineoutputs, dry_run=True, batch_size=601)
        assert_raises_rpc_error(-8, "min_amount", dust_w.combineoutputs, dry_run=True, min_amount=10, max_amount=10)

        self.log.info("A real run spends exactly the reported outputs, to one new address, without change")
        preview = dust_w.combineoutputs(dry_run=True, batch_size=10)
        res = dust_w.combineoutputs(dry_run=False, batch_size=10)
        assert_equal(res["dry_run"], False)
        address = res["address"]
        assert dust_w.getaddressinfo(address)["ismine"]
        assert_equal(len(res["batches"]), 3)
        spent = set()
        mempool = set(node.getrawmempool())
        for estimate, b in zip(preview["batches"], res["batches"]):
            assert "error" not in b, b
            assert_equal(b["status"], "submitted")
            assert estimate["size"] >= b["size"]
            assert_equal(estimate["size"], estimate["estimated_signed_size"])
            assert_equal(b["stakeable_amounts"], True)
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
        res = pieces_w.combineoutputs(dry_run=False, output_size=Decimal("300"), staking_only=True)
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

        def previews_take_nothing():
            pool = enc_w.getwalletinfo()
            descriptors = enc_w.listdescriptors() if self.options.descriptors else None
            for _ in range(3):
                res = enc_w.combineoutputs()
                # each preview builds its batch: the three 4 DFCN coins less the fee, in one output
                assert_equal(res["candidates"], 3)
                assert_equal(len(res["batches"]), 1)
                batch = res["batches"][0]
                assert "error" not in batch, batch
                assert_equal(batch["inputs"], 3)
                assert_equal([Decimal(str(v)) for v in batch["outputs"]], [Decimal("12") - Decimal(str(batch["fee"]))])
            for field in ("keypoolsize", "keypoolsize_hd_internal"):
                assert_equal(pool.get(field), enc_w.getwalletinfo().get(field))
            if descriptors is not None:
                assert_equal(descriptors, enc_w.listdescriptors())
            return pool

        previews_take_nothing()
        assert_raises_rpc_error(-13, "passphrase", enc_w.combineoutputs, dry_run=False)
        # nothing was sent by the refused call
        assert_equal(len(self.unspent(enc_w, 1)), 3)
        if not self.options.descriptors:
            # the locked legacy wallet's key pools used up, external and internal: the previews still take nothing
            # and top nothing up
            while enc_w.getwalletinfo()["keypoolsize"] > 0:
                enc_w.getnewaddress()
            while enc_w.getwalletinfo()["keypoolsize_hd_internal"] > 0:
                enc_w.getrawchangeaddress()
            assert_raises_rpc_error(-12, "Keypool ran out", enc_w.getnewaddress)
            assert_raises_rpc_error(-12, "Keypool ran out", enc_w.getrawchangeaddress)
            empty = previews_take_nothing()
            assert_equal(empty["keypoolsize"], 0)
            assert_equal(empty["keypoolsize_hd_internal"], 0)
        enc_w.walletpassphrase('pass', 60, True)
        assert_equal(enc_w.combineoutputs()['candidates'], 3)
        assert_raises_rpc_error(-13, 'passphrase', enc_w.combineoutputs, dry_run=False)
        enc_w.walletlock()

        self.test_coinjoin_amount_pieces(node, funder, mine_to)

        self.log.info("A rejected batch reports abandonment only after its inputs are released")
        node.createwallet(wallet_name="rejected", descriptors=self.options.descriptors)
        rejected = node.get_wallet_rpc("rejected")
        rejected.keypoolrefill(10)
        self.fund(funder, rejected.getnewaddress(), Decimal("60"))
        self.generatetoaddress(node, 1, mine_to)
        self.restart_node(0, extra_args=["-limitancestorcount=1", "-walletrejectlongchains=0"])
        node = self.nodes[0]
        if "rejected" not in node.listwallets():
            node.loadwallet("rejected")
        rejected = node.get_wallet_rpc("rejected")
        parent = rejected.sendtoaddress(rejected.getnewaddress(), Decimal("5"))
        assert parent in node.getrawmempool()
        # The CPFP carve-out permits a two-transaction chain even at limit 1.
        # A third transaction exceeds that exception as well.
        child = rejected.sendtoaddress(rejected.getnewaddress(), Decimal('10'))
        assert_equal(node.getmempoolancestors(child), [parent])
        before = self.unspent(rejected)
        res = rejected.combineoutputs(dry_run=False, minconf=0)
        failed = res["batches"][0]
        assert_equal(failed["status"], "rejected_abandoned")
        assert "error" in failed
        assert failed["txid"] not in node.getrawmempool()
        assert_equal(self.unspent(rejected), before)
        assert rejected.gettransaction(failed["txid"])["details"][0]["abandoned"]

        self.log.info("Broadcast disabled records the transaction without claiming submission")
        self.generatetoaddress(node, 1, mine_to)
        self.restart_node(0, extra_args=["-walletbroadcast=0"])
        node = self.nodes[0]
        if "rejected" not in node.listwallets():
            node.loadwallet("rejected")
        rejected = node.get_wallet_rpc("rejected")
        res = rejected.combineoutputs(dry_run=False)
        batch = res["batches"][0]
        assert_equal(batch["status"], "recorded")
        assert "error" not in batch
        assert batch["txid"] not in node.getrawmempool()


if __name__ == "__main__":
    WalletCombineOutputsTest().main()
