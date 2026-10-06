#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Continuous stake consolidation: disabled control, default cross-key inputs,
rest budget, excluded outputs, stable layout, and observer reindex. Run both
legacy and descriptor variants. --key-only checks the privacy-preserving scope;
--observer-binary can use an unchanged validator from the previous build."""

import os
import time
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    force_finish_mnsync,
    set_node_times,
)

# chainparams.cpp, CRegTestParams
LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
REGULAR_MN_COLLATERAL = Decimal("1000")
# pos/stake.h: MIN_STAKE_TARGET, the size regtest gets (it has no stake floor
# and no proof-of-stake history for netstakeweight to measure); read back from
# getstakinginfo before anything is computed from it
MIN_STAKE_TARGET = Decimal("20000")
MAX_STAKE_COMBINE_INPUTS = 20
POS_REWARD = Decimal("500")
# the kernel's depth rule, in the wallet's terms (ClassifyForStaking)
COINBASE_MATURITY = 25
KERNEL_DEPTH = COINBASE_MATURITY + 2

PIECE = Decimal("20000")          # not a collateral amount on regtest
SAME_KEY_PIECES = 64
# The shallow output and the other key's are smaller than every piece, so that
# combining -- smallest first -- would reach them before any piece if the rule
# that keeps them out were missing. At equal amounts the order among them is
# the wallet's own, and a broken rule could pass by luck.
DECOY = Decimal("1400")
BIG_COIN = Decimal("70000")      # three targets and a half

POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
STAKE_CLOCK_STEP = 64
STAKE_TIMEOUT = 300


class PosStakeCombineTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            ["-staking=1", "-txindex=1", "-debug=pos", "-stakecombine=0"],
            ["-staking=0"],
        ]

    def add_options(self, parser):
        parser.add_argument("--key-only", action="store_true")
        parser.add_argument("--observer-binary", default=os.environ.get("STAKE_TEST_OBSERVER_BINARY"))

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    # ----- clock and chain ---------------------------------------------------

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def mine_pow_to(self, node, height, address):
        while node.getblockcount() < height:
            todo = min(POW_CHUNK, height - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.sync_blocks)
        assert_equal(node.getblockcount(), height)

    def settled_height(self, node, quiet=4, timeout=60):
        height = node.getblockcount()
        quiet_since = time.time()
        deadline = time.time() + timeout * self.options.timeout_factor
        while time.time() < deadline:
            time.sleep(0.5)
            current = node.getblockcount()
            if current != height:
                height, quiet_since = current, time.time()
            elif time.time() - quiet_since >= quiet:
                return height
        raise AssertionError("the block count kept moving after staking was switched off")

    # ----- staking wallets ---------------------------------------------------

    def wallet_id(self, node, name, timeout=30):
        # the minter's maintenance adds a newly loaded wallet on its next round
        deadline = time.time() + timeout * self.options.timeout_factor
        while True:
            for wid, entry in node.liststakingwallets().items():
                if entry["name"] == name:
                    return wid
            if time.time() > deadline:
                raise AssertionError("wallet %r is not a staking wallet: %s" % (name, node.liststakingwallets()))
            time.sleep(0.5)

    def staking_info(self, node, name, timeout=30):
        # By name, from the answer itself: getstakinginfo is keyed by position
        # in a list the minter rebuilds, and a wallet loaded a moment ago may
        # not be in it yet.
        deadline = time.time() + timeout * self.options.timeout_factor
        while True:
            for entry in node.getstakinginfo().values():
                if entry["name"] == name:
                    return entry
            if time.time() > deadline:
                raise AssertionError("wallet %r not in getstakinginfo: %s" % (name, node.getstakinginfo()))
            time.sleep(0.5)

    def set_staking(self, node, name, on):
        wid = self.wallet_id(node, name)
        if node.liststakingwallets()[wid]["enabled"] != on:
            # setstaking is a toggle
            assert_equal(node.setstaking(int(wid)), on)
        assert_equal(self.staking_info(node, name)["staking"], on)
        if on:
            self.wait_until(lambda: self.staking_info(node, name)["minter_running"], timeout=30)

    def stake_until(self, node, name, done, max_blocks):
        """Stake with `name` alone until `done(block)` holds for a new block,
        at most `max_blocks` blocks; switch staking off, wait for the height to
        settle, and return every block the phase produced (verbosity 2)."""
        start = node.getblockcount()
        self.set_staking(node, name, True)
        found = False
        deadline = time.time() + STAKE_TIMEOUT * self.options.timeout_factor
        seen = start
        while not found:
            assert time.time() < deadline, "phase %s staked no qualifying block; %s" % (name, self.staking_info(node, name))
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
            while seen < node.getblockcount():
                seen += 1
                if done(node.getblock(node.getblockhash(seen), 2)):
                    found = True
                    break
            assert seen - start <= max_blocks, "phase %s: %d blocks and none qualified" % (name, seen - start)
        self.set_staking(node, name, False)
        final = self.settled_height(node)
        return [node.getblock(node.getblockhash(h), 2) for h in range(start + 1, final + 1)]

    # ----- block checks ------------------------------------------------------

    @staticmethod
    def outpoint(vin):
        return (vin["txid"], vin["vout"])

    def check_coinstake(self, node, block):
        """The shape every staked block must have, whatever the phase."""
        assert_equal(block["flags"], "proof-of-stake")
        coinstake = block["tx"][1]
        assert_equal(coinstake["vout"][0]["value"], 0)
        assert_equal(coinstake["vout"][0]["scriptPubKey"]["hex"], "")
        # every piece goes to the kernel's pay-to-pubkey script
        pieces = coinstake["vout"][1:]
        assert_greater_than(len(pieces), 0)
        for out in pieces:
            assert_equal(out["scriptPubKey"]["type"], "pubkey")
            assert_equal(out["scriptPubKey"]["hex"], pieces[0]["scriptPubKey"]["hex"])
        # exactly the reward is minted, however many inputs were spent
        value_in = Decimal(0)
        for vin in coinstake["vin"]:
            prev = node.getrawtransaction(vin["txid"], True)
            value_in += prev["vout"][vin["vout"]]["value"]
        assert_equal(sum(out["value"] for out in pieces) - value_in, POS_REWARD)
        return coinstake

    # ----- funding -----------------------------------------------------------

    def fund(self, funder, address, amount):
        """One output of `amount` to `address`; returns its outpoint."""
        txid = funder.sendtoaddress(address, amount)
        tx = funder.getrawtransaction(txid, True)
        for out in tx["vout"]:
            spk = out["scriptPubKey"]
            if out["value"] == amount and (spk.get("address") == address or address in spk.get("addresses", [])):
                return (txid, out["n"])
        raise AssertionError("funding output not found in %s" % txid)

    # ----- the test ----------------------------------------------------------

    def run_test(self):
        staker, observer = self.nodes[0], self.nodes[1]
        if self.options.observer_binary:
            self.stop_node(1)
            observer.binary = self.options.observer_binary
            self.start_node(1)
            self.connect_nodes(0, 1)
        if self.mocktime == 0:
            self.mocktime = staker.getblockheader(staker.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        funder = staker.get_wallet_rpc(self.default_wallet_name)
        mine_to = funder.getnewaddress()

        for name in ("off", "combine", "split"):
            staker.createwallet(wallet_name=name, descriptors=self.options.descriptors)
        w_off = staker.get_wallet_rpc("off")
        w_comb = staker.get_wallet_rpc("combine")
        w_split = staker.get_wallet_rpc("split")

        self.log.info("Mining proof-of-work blocks, funding the three staking wallets on the way")
        self.mine_pow_to(staker, LAST_POW_BLOCK - 100, mine_to)

        # phase 1: five equal outputs on one key
        off_addr = w_off.getnewaddress()
        off_outs = {self.fund(funder, off_addr, PIECE) for _ in range(5)}

        # A sufficiently funded pool leaves room for gradual consolidation
        # within the default 5% resting-value budget.
        key_a = w_comb.getnewaddress()
        key_b = w_comb.getnewaddress()
        same_key = {self.fund(funder, key_a, PIECE) for _ in range(SAME_KEY_PIECES)}
        locked = self.fund(funder, key_a, DECOY)
        collateral = self.fund(funder, key_a, REGULAR_MN_COLLATERAL)
        other_key = self.fund(funder, key_b, DECOY)

        # phase 3: one output of three and a half targets
        split_addr = w_split.getnewaddress()
        big = self.fund(funder, split_addr, BIG_COIN)

        self.generatetoaddress(staker, 1, mine_to, sync_fun=self.sync_blocks)
        assert_equal(staker.getrawmempool(), [])
        assert w_comb.lockunspent(False, [{"txid": locked[0], "vout": locked[1]}])

        # too shallow when the phases run: funded close to the boundary
        self.mine_pow_to(staker, LAST_POW_BLOCK - 5, mine_to)
        young = self.fund(funder, key_a, DECOY)
        self.mine_pow_to(staker, LAST_POW_BLOCK, mine_to)
        assert_equal(staker.getrawmempool(), [])
        assert_equal(w_comb.gettransaction(young[0])["confirmations"], 5)

        for node in self.nodes:
            force_finish_mnsync(node)
        tip_time = staker.getblockheader(staker.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)

        # ---- phase 1 --------------------------------------------------------
        self.log.info("Phase 1: with -stakecombine=0 a win spends only its kernel")
        info = self.staking_info(staker, "off")
        assert_equal(info["stake_combine"], False)
        blocks = self.stake_until(staker, "off", lambda b: True, max_blocks=3)
        for block in blocks:
            coinstake = self.check_coinstake(staker, block)
            assert_equal(len(coinstake["vin"]), 1)
            assert self.outpoint(coinstake["vin"][0]) in off_outs
        # other outputs of the same key were there to be taken, and were not
        assert_greater_than(len(off_outs), len(blocks))
        self.sync_blocks()

        # ---- restart with combining on (the default) -------------------------
        self.log.info("Restarting the staking node with the defaults")
        self.restart_node(0, extra_args=["-staking=1", "-txindex=1", "-debug=pos"] + (["-stakecombinescope=key"] if self.options.key_only else []))
        for name in ("combine", "split"):
            if name not in staker.listwallets():
                staker.loadwallet(name)
        w_comb = staker.get_wallet_rpc("combine")
        w_split = staker.get_wallet_rpc("split")
        # lockunspent is not stored unless asked; lock it again after the restart
        assert w_comb.lockunspent(False, [{"txid": locked[0], "vout": locked[1]}])
        self.connect_nodes(0, 1)
        force_finish_mnsync(staker)
        self.sync_blocks()

        # ---- phase 2 --------------------------------------------------------
        self.log.info("Phase 2: continuous consolidation respects scope and the wallet rest budget")
        info = self.staking_info(staker, "combine")
        assert_equal(info["stake_combine"], True)
        assert_equal(info["stake_combine_scope"], "key" if self.options.key_only else "wallet")
        assert_equal(info["stake_compact_target"], 2 * MIN_STAKE_TARGET)
        assert_equal(info["stake_split_threshold"], 3 * MIN_STAKE_TARGET)
        assert_equal(info["stake_target_configured"], False)
        target = Decimal(str(info["stake_target"]))
        assert_equal(target, MIN_STAKE_TARGET)
        # The regular pieces, the collateral, the shallow output and the other
        # key's: the locked output is not available to spend, so not counted
        assert_equal(info["stake_outputs"], SAME_KEY_PIECES + 3)

        never = {locked, collateral, young}

        def combines(block):
            return len(block["tx"][1]["vin"]) > 1

        blocks = self.stake_until(staker, "combine", combines, max_blocks=4)
        combined_blocks = 0
        all_spent = set()
        for block in blocks:
            coinstake = self.check_coinstake(staker, block)
            spent = [self.outpoint(v) for v in coinstake["vin"]]
            all_spent.update(spent)
            assert len(spent) <= MAX_STAKE_COMBINE_INPUTS
            for op in spent:
                assert op not in never
            if len(spent) > 1:
                combined_blocks += 1
                assert len(coinstake["vout"]) - 1 < len(spent)
                assert_equal(len(coinstake["vout"]), 2)
                if self.options.key_only:
                    assert other_key not in spent
                details = w_comb.gettransaction(coinstake["txid"])["stake_details"]
                assert_equal(details["confirmed"], True)
                assert_equal(details["input_details_complete"], True)
                assert_equal(details["net_reward"], POS_REWARD)
                assert_equal(details["input_count"], len(spent))
                assert_equal(details["output_count"], 1)
                assert_equal(details["utxo_delta"], 1 - len(spent))
                assert_equal(details["inputs"][0]["role"], "winning_stake")
                assert_equal(details["principal"] + POS_REWARD, details["output_total"])
                self.log.info("block %d: %d inputs -> %d outputs", block["height"], len(spent), len(coinstake["vout"]) - 1)
        assert_greater_than(combined_blocks, 0)
        if not self.options.key_only:
            assert other_key in all_spent

        # At-target inputs must also combine into one piece above the OLD
        # splitting boundary. Re-stake that actual output after it matures.
        compact_tx = next((b["tx"][1] for b in blocks if len(b["tx"][1]["vin"]) > 1
                           and b["tx"][1]["vout"][1]["value"] >= 2 * target), None)
        if compact_tx is None:
            more = self.stake_until(staker, "combine", lambda b: len(b["tx"][1]["vin"]) > 1
                                    and b["tx"][1]["vout"][1]["value"] >= 2 * target, max_blocks=3)
            for block in more:
                tx = self.check_coinstake(staker, block)
                if len(tx["vin"]) > 1 and tx["vout"][1]["value"] >= 2 * target:
                    compact_tx = tx
            blocks += more
        assert compact_tx is not None
        assert_equal(len(compact_tx["vout"]), 2)

        # the four that must never be combined are all still unspent
        unspent = {(u["txid"], u["vout"]) for u in w_comb.listunspent(0)}
        for op in (collateral, young):
            assert op in unspent
        locked_list = {(u["txid"], u["vout"]) for u in w_comb.listlockunspent()}
        assert locked in locked_list
        assert w_comb.gettxout(locked[0], locked[1]) is not None
        if other_key not in {self.outpoint(v) for b in blocks for v in b["tx"][1]["vin"]}:
            assert other_key in unspent
        self.sync_blocks()

        # ---- phase 3 --------------------------------------------------------
        self.log.info("Phase 3: a credit of three targets and more is split into equal pieces")
        blocks = self.stake_until(staker, "split", lambda b: True, max_blocks=1)
        assert_equal(len(blocks), 1)
        coinstake = self.check_coinstake(staker, blocks[0])
        assert_equal([self.outpoint(v) for v in coinstake["vin"]], [big])
        credit = BIG_COIN + POS_REWARD
        pieces = max(2, int(credit // (2 * target)))
        assert_equal(pieces, 2)
        values = [o["value"] for o in coinstake["vout"][1:]]
        assert_equal(len(values), pieces)
        assert_equal(sum(values), credit)
        assert_equal(len(set(values)), 1)
        self.log.info("block %d: %s split into %s", blocks[0]["height"], credit, values)

        # Disconnect/reconnect a real coinstake block. Details describe its
        # recorded inputs even while it is no longer confirmed in this chain.
        split_txid = coinstake["txid"]
        detached_tip = staker.getbestblockhash()
        staker.invalidateblock(detached_tip)
        assert_equal(w_split.gettransaction(split_txid)["stake_details"]["confirmed"], False)
        staker.reconsiderblock(detached_tip)
        self.wait_until(lambda: staker.getbestblockhash() == detached_tip)
        assert_equal(w_split.gettransaction(split_txid)["stake_details"]["confirmed"], True)

        # Advance this isolated chain with another wallet, leaving the
        # compact output alone until both depth and age have passed.
        mature_height = staker.getblockcount() + KERNEL_DEPTH
        self.stake_until(staker, self.default_wallet_name,
                         lambda b: b["height"] >= mature_height, max_blocks=KERNEL_DEPTH + 2)
        compact_op = (compact_tx["txid"], 1)
        # Exclude the other spendable coins so the intended output must win.
        to_lock = [{"txid": u["txid"], "vout": u["vout"]} for u in w_comb.listunspent()
                   if (u["txid"], u["vout"]) != compact_op]
        assert w_comb.lockunspent(False, to_lock)
        stable_blocks = self.stake_until(staker, "combine", lambda b: True, max_blocks=1)
        assert_equal(len(stable_blocks), 1)
        stable_tx = self.check_coinstake(staker, stable_blocks[0])
        assert_equal([self.outpoint(v) for v in stable_tx["vin"]], [compact_op])
        assert_equal(len(stable_tx["vout"]), 2)
        assert_equal(stable_tx["vout"][1]["value"], compact_tx["vout"][1]["value"] + POS_REWARD)
        self.log.info("The compact output won again and remained a single output")

        # ---- the observer --------------------------------------------------
        self.log.info("The non-staking node accepted every block, and re-validates them from disk")
        self.sync_blocks()
        final_height = staker.getblockcount()
        final_hash = staker.getbestblockhash()
        assert_equal(observer.getbestblockhash(), final_hash)
        self.restart_node(1, extra_args=self.extra_args[1] + ["-reindex"])
        self.wait_until(lambda: observer.getblockcount() == final_height, timeout=600)
        assert_equal(observer.getbestblockhash(), final_hash)

        # ---- -staketarget ---------------------------------------------------
        self.log.info("-staketarget replaces the derived size and is reported as configured")
        self.restart_node(0, extra_args=["-staking=1", "-txindex=1", "-staketarget=50000"])
        if "combine" not in staker.listwallets():
            staker.loadwallet("combine")
        info = self.staking_info(staker, "combine")
        assert_equal(Decimal(str(info["stake_target"])), Decimal("50000"))
        assert_equal(info["stake_target_configured"], True)


if __name__ == "__main__":
    PosStakeCombineTest().main()
