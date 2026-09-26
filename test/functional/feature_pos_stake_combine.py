#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stake coinstakes that combine outputs and split into equal pieces, and have
a second node accept them.

A coinstake used to spend exactly one input, the kernel, and to pay at most
two outputs. The wallet now also spends some of the kernel key's small outputs
in the same coinstake -- at no fee, since a coinstake pays none -- and lays the
credit out in pieces of about a target size. Consensus has always allowed both
shapes (CheckProofOfStake judges vin[0] alone; every further input is an
ordinary spend, and the reward ceiling counts every input), but no block on any
chain has carried either, so this test is where they are first built by a real
node and validated by another.

Three wallets on the staking node, one per phase, each staking alone:

1. combining switched off (-stakecombine=0): a win spends only its kernel,
   although the same key holds other outputs that would qualify -- the
   negative control for the feature itself;
2. combining on: one key holds fourteen equal outputs, next to one output of
   each kind that must never be combined -- locked with lockunspent, exactly a
   masternode collateral amount, too shallow, and held by another key of the
   same wallet. A win by one of the fourteen spends thirteen of them: the
   fourteenth would take the credit past the target. None of the four ever
   appears in a coinstake;
3. a single output three times the target: the win is split into three equal
   pieces, all paid to the kernel's pay-to-pubkey script.

Every coinstake must mint exactly the reward, and every block must be accepted
over P2P by a node that does not stake and then survive that node's -reindex.

The clock handling follows feature_pos_staking.py, whose notes apply here.
"""

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

PIECE = Decimal("1500")          # not a collateral amount on regtest
SAME_KEY_PIECES = 14
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

        # phase 2: fourteen equal outputs on one key, and one of each kind that
        # must never be combined
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
        self.restart_node(0, extra_args=["-staking=1", "-txindex=1", "-debug=pos"])
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
        self.log.info("Phase 2: a win combines the kernel key's outputs up to the target")
        info = self.staking_info(staker, "combine")
        assert_equal(info["stake_combine"], True)
        assert_equal(info["stake_target_configured"], False)
        target = Decimal(str(info["stake_target"]))
        assert_equal(target, MIN_STAKE_TARGET)
        # the fourteen, the collateral amount, the shallow one and the other
        # key's: the locked output is not available to spend, so not counted
        assert_equal(info["stake_outputs"], SAME_KEY_PIECES + 3)

        never = {locked, collateral, young}

        def combines(block):
            return len(block["tx"][1]["vin"]) > 1

        blocks = self.stake_until(staker, "combine", combines, max_blocks=4)
        # the kernel and twelve more reach 19,500; a thirteenth would pass the
        # target. A later win in the same phase finds fewer left to take.
        by_target = int((target - PIECE) // PIECE)
        assert_equal(by_target, 12)
        left = set(same_key)
        full_combines = 0
        for block in blocks:
            coinstake = self.check_coinstake(staker, block)
            spent = [self.outpoint(v) for v in coinstake["vin"]]
            kernel, extra = spent[0], spent[1:]
            for op in spent:
                assert op not in never, "coinstake %s spent %s, which must never be combined" % (coinstake["txid"], op)
            if kernel == other_key:
                # the other key has nothing of its own to combine
                assert_equal(extra, [])
                continue
            assert kernel in left
            left.discard(kernel)
            # only the kernel key's outputs, never the other key's
            assert other_key not in extra
            for op in extra:
                assert op in left
                left.discard(op)
            expected_extra = min(MAX_STAKE_COMBINE_INPUTS - 1, by_target, len(left) + len(extra))
            assert_equal(len(extra), expected_extra)
            credit = PIECE * (1 + len(extra)) + POS_REWARD
            # under twice the target the credit stays whole
            assert_equal([o["value"] for o in coinstake["vout"][1:]], [credit])
            if len(extra) == by_target:
                full_combines += 1
            self.log.info("block %d: coinstake %s spent %d inputs into one output of %s",
                          block["height"], coinstake["txid"], len(spent), credit)
        assert_equal(full_combines, 1)

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
        pieces = int(credit // target)
        assert_equal(pieces, 3)
        values = [o["value"] for o in coinstake["vout"][1:]]
        assert_equal(len(values), pieces)
        assert_equal(sum(values), credit)
        assert_equal(len(set(values)), 1)
        self.log.info("block %d: %s split into %s", blocks[0]["height"], credit, values)

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
