#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""-testactivationheight=v23@H on a proof-of-stake chain: the staking half of the bundle.

The v23 bundle writes every one of its heights from a single number, and
regtest starts with the five proof-of-stake rules already active from height 0,
so every existing staking test runs entirely on the "after" side of them. Under
the bundle's own name they sit at H, and a chain that stakes through H has the
"before" rules for its first stretch and the "after" rules from H on, on one
chain, in blocks that nodes must all accept, restart into and rebuild:

  5001 .. H-1   proof-of-stake blocks under the rules a running network has
  H             the same chain, on the rules the release switches on
  H .. H+30     blocks keep coming, and a node that saw none of them follows

Nothing about an honest block changes at H except one thing that can be read
off it: the stake modifier. Until the gate a block's modifier is
Hash(kernel || previous modifier) with a kernel that was never filled in --
32 zero bytes, so it is a function of the height alone; from the gate the
kernel is the transaction id the block staked (ComputeStakeModifier,
pos/kernel.cpp; the write in ConnectBlock, validation.cpp). getblock prints the
modifier, so the test recomputes it for every proof-of-stake block, in Python,
from the rule of that block's own side, and the switch has to fall at H and
nowhere else.

The other four rules (nonce, block time, coinbase bound, fee burning) refuse
blocks that an honest wallet does not produce. They are refusals, tested where
a block can be built to break them (the unit suites); a functional chain can
show only that honest blocks go on being accepted on both sides. What it can
show about fees is the accounting: a transaction with a known fee is mined in
block H-1 and another in block H, and on both sides the coinstake mints the
subsidy and no more, so the fee leaves the money supply -- the supply grows by
the subsidy less the fee.

The tip is then walked back and forth across H, restarted, rebuilt from its own
block files, and a node that has never seen the chain syncs it from genesis.
After each, the modifiers around H and at the tip, and the UTXO set, are the
same as before.

Three nodes: `staker` stakes, `peer` follows over P2P, `late` starts with the
chain finished and syncs it. The clock is the mock clock throughout, because a
stake attempt is keyed to a search time that must be strictly after the tip's
time, and a coin can only stake once its block is older than stakeAgeRange[0].

Not in test_runner.py: one run takes about eleven minutes, most of it the
minter's real-time sleeps between staked blocks.
Run it alone: test/functional/feature_v23_joint_activation_pos.py --descriptors
"""

import time
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.messages import hash256
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
POS_TIMESTAMP_MASK = 5
POS_REWARD = Decimal("500")           # GetProofOfStakeReward()

# A staked block costs about 5.9 s of REAL time whatever the mock clock does: the minter sleeps 2.5 s at the top of
# every round and 2.5 s more after a block it has found (pos/minter.cpp:215 and :364, CStakeWallet::SHORTDELAY).
# The rehearsal plan named H = 5160, which is 159 staked blocks before H and 19 minutes of staking for the run to get
# there; H = 5064 is 63 blocks and 6, and is still 211 x 24, with H + 576 = 5640 on the 24-block epoch grid.
H = 5064
AFTER = 30                            # blocks staked past H
MODIFIER_WINDOW = 3                   # blocks either side of H whose modifiers are compared after each event

POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
STAKE_CLOCK_STEP = 64                 # one search window per step; clears the wallet's 60 s park as well
STAKE_TIMEOUT = 300                   # wall clock, one block; scaled by --timeout-factor
TX_AGE = 130                          # WAIT_FOR_ISLOCK_TIMEOUT (120 s) plus a margin: no quorum exists to lock a transaction
# A node that has never seen the chain takes all of it, about 5100 blocks. It validated them at about 90 blocks a second
# on a machine with a compile running beside it, which is a minute: the framework's 60 s default is not sized for that.
SYNC_TIMEOUT = 300

FEE_BEFORE = Decimal("0.001")         # the transaction mined in block H-1
FEE_AT = Decimal("0.0025")            # the transaction mined in block H

BUNDLE = ["-testactivationheight=v23@%d" % H]


def modifier_of(prev_modifier, kernel_txid):
    """ComputeStakeModifier: Hash(kernel || previous modifier), each serialised as a uint256.

    RPC prints a uint256 byte-reversed, so both inputs are reversed back and
    the result is reversed for printing. kernel_txid None is the gate's
    "before": the 32 zero bytes the kernel still was.
    """
    kernel = bytes(32) if kernel_txid is None else bytes.fromhex(kernel_txid)[::-1]
    return hash256(kernel + bytes.fromhex(prev_modifier)[::-1])[::-1].hex()


class V23JointActivationPosTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [
            ["-staking=1", "-txindex=1", "-debug=pos"] + BUNDLE,   # staker
            ["-staking=0"] + BUNDLE,                                # peer
            ["-staking=0"] + BUNDLE,                                # late
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    def setup_network(self):
        self.setup_nodes()
        # `late` stays alone until the chain is finished
        self.connect_nodes(0, 1)

    # ----- helpers ---------------------------------------------------------

    def mark(self, text):
        self.log.info("[%4d s, height %d] %s", time.time() - self.started, self.nodes[0].getblockcount(), text)

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def sync_pair(self):
        self.sync_blocks(self.nodes[:2])

    def mine_to_boundary(self, node, address):
        self.log.info("Mining proof-of-work blocks up to lastPowBlock (%d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.sync_pair)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)
        assert_equal(node.getblock(node.getbestblockhash())["flags"], "proof-of-work")

    def assert_pow_refused_above_boundary(self, node, address):
        """Without this a fixture that merely reaches height 5000 proves nothing about the regime it is in."""
        before = node.getblockcount()
        try:
            self.generatetoaddress(node, 1, address, sync_fun=self.no_op)
        except JSONRPCException as e:
            message = e.error["message"]
            assert ("bad-pos-nonce" in message) or ("pow-late" in message), message
        else:
            raise AssertionError("a proof-of-work block was accepted above lastPowBlock")
        assert_equal(node.getblockcount(), before)

    def staking_info(self, node):
        return node.getstakinginfo()[str(self.wallet_id)]

    def enable_staking(self, node):
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        self.wallet_id = int(next(iter(wallets)))
        assert_equal(node.setstaking(self.wallet_id), True)   # a toggle: True is "now on"
        self.wait_until(lambda: self.staking_info(node)["minter_running"], timeout=30)

    def disable_staking(self, node):
        assert_equal(node.setstaking(self.wallet_id), False)

    def settled_height(self, node, quiet=4, timeout=60):
        """The block count once it has stopped moving: with staking off and the clock frozen at most one more
        block can arrive, an attempt the minter had already started."""
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

    def stake_exactly_to(self, node, height):
        """Stake until the chain holds `height`, switch staking off, and prove that not one block more arrived.

        An overrun is a test failure, never a re-addressed target: the transactions below are placed by the
        height they are mined at.
        """
        self.enable_staking(node)
        # the bound is per block, not per call: one block takes about 6 s of real time (see H above)
        last_progress = deadline = None
        while node.getblockcount() < height:
            now_height = node.getblockcount()
            if now_height != last_progress:
                last_progress = now_height
                deadline = time.time() + STAKE_TIMEOUT * self.options.timeout_factor
                if now_height % 20 == 0:
                    self.mark("staking")
            assert time.time() < deadline, "no block above %d in %d s; getstakinginfo: %s" % (
                now_height, STAKE_TIMEOUT, self.staking_info(node))
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        self.disable_staking(node)
        assert_equal(self.settled_height(node), height)
        self.sync_pair()
        return node.getblockhash(height)

    def send_fee_tx(self, node, fee):
        """A transaction with a known fee, from a mature output the wallet is told not to stake.

        The wallet is not left to pick its own inputs: it stakes, and coin selection has been seen to choose
        outputs that are not spendable yet, which gives a txid and never a mempool entry. So the input is
        named, and the mempool is asked afterwards.
        """
        utxo = node.listunspent(300, 9999999, [], True, {"maximumCount": 1})[0]
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        raw = node.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}],
                                        {node.getnewaddress(): utxo["amount"] - fee})
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"]
        txid = node.sendrawtransaction(signed["hex"])
        assert txid in node.getrawmempool(), "the transaction was accepted but is not in the mempool"
        assert_equal(Decimal(str(node.getmempoolentry(txid)["fees"]["base"])), fee)
        return txid

    def supply(self, node):
        return Decimal(str(node.gettxoutsetinfo()["total_amount"]))

    def coinstake_reward(self, node, coinstake):
        value_in = Decimal(0)
        for vin in coinstake["vin"]:
            prev = node.getrawtransaction(vin["txid"], True)
            value_in += prev["vout"][vin["vout"]]["value"]
        return sum(out["value"] for out in coinstake["vout"]) - value_in

    def check_modifiers(self, node, first, last):
        """Recompute the stake modifier of every proof-of-stake block in [first, last] from its own side of H.

        The modifier of `first` is taken as read; every later one must be Hash(kernel || previous), with the
        kernel of that block's own side. Returns how many blocks were checked on each side.
        """
        before = after = 0
        prev = node.getblock(node.getblockhash(first))["modifier"]
        for height in range(first + 1, last + 1):
            block = node.getblock(node.getblockhash(height), 2)
            assert_equal(block["flags"], "proof-of-stake")
            kernel = block["tx"][1]["vin"][0]["txid"] if height >= H else None
            expected = modifier_of(prev, kernel)
            assert_equal(block["modifier"], expected)
            if height >= H:
                after += 1
            else:
                before += 1
            prev = block["modifier"]
        return before, after

    def snapshot(self, node):
        """What every later event must leave as it was: the tip, the modifiers around H and at the tip, the UTXO set."""
        height = node.getblockcount()
        wanted = list(range(H - MODIFIER_WINDOW, H + MODIFIER_WINDOW + 1)) + [height]
        info = node.gettxoutsetinfo()
        return {
            "tip": node.getbestblockhash(),
            "height": height,
            "modifiers": {h: node.getblock(node.getblockhash(h))["modifier"] for h in wanted},
            "utxo": info["hash_serialized_2"],
            "supply": Decimal(str(info["total_amount"])),
        }

    def assert_same(self, node, expected, what):
        assert_equal(node.getbestblockhash(), expected["tip"])
        assert_equal(node.getblockcount(), expected["height"])
        now = self.snapshot(node)
        assert_equal(now["modifiers"], expected["modifiers"])
        assert_equal(now["utxo"], expected["utxo"])
        self.log.info("  %s: the same tip, the same modifiers around H and at the tip, the same UTXO set", what)

    # ----- the test --------------------------------------------------------

    def run_test(self):
        staker, peer, late = self.nodes
        self.started = time.time()
        if self.mocktime == 0:
            self.mocktime = staker.getblockheader(staker.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        address = staker.getnewaddress()

        self.log.info("Every node runs under the bundle's name, -testactivationheight=v23@%d. No RPC prints the "
                      "proof-of-stake heights: that the switch falls at H is shown by the modifier check below", H)
        assert_equal(H % 24, 0)
        assert_equal((H + 576) % 24, 0)

        self.mine_to_boundary(staker, address)
        self.assert_pow_refused_above_boundary(staker, address)
        self.mark("proof-of-work era mined; a proof-of-work block above the boundary is refused")

        # the masternode-sync gate of the minter FIRST, and only then any staking; and the coin-age rule
        force_finish_mnsync(staker)
        force_finish_mnsync(peer)
        tip_time = staker.getblockheader(staker.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)

        self.log.info("Staking to H - 2 = %d", H - 2)
        self.stake_exactly_to(staker, H - 2)
        self.mark("staked to H - 2")
        supply_h2 = self.supply(staker)

        # ---- H - 1: a transaction with a known fee, mined in exactly this block ---------------------------
        self.log.info("Block H - 1 = %d carries a transaction with a fee of %s", H - 1, FEE_BEFORE)
        tx_before = self.send_fee_tx(staker, FEE_BEFORE)
        self.advance_clock(TX_AGE)
        h1_hash = self.stake_exactly_to(staker, H - 1)
        assert tx_before in staker.getblock(h1_hash)["tx"], "the transaction was not mined in block H - 1"
        supply_h1 = self.supply(staker)

        # ---- H: another one, mined in the first block under the new rules ----------------------------------
        self.log.info("Block H = %d carries a transaction with a fee of %s", H, FEE_AT)
        tx_at = self.send_fee_tx(staker, FEE_AT)
        self.advance_clock(TX_AGE)
        h_hash = self.stake_exactly_to(staker, H)
        assert tx_at in staker.getblock(h_hash)["tx"], "the transaction was not mined in block H"
        supply_h = self.supply(staker)
        self.mark("blocks H - 1 and H staked, each with its transaction")

        self.log.info("The fee leaves the money supply on both sides: it grows by the subsidy less the fee")
        assert_equal(supply_h1 - supply_h2, POS_REWARD - FEE_BEFORE)
        assert_equal(supply_h - supply_h1, POS_REWARD - FEE_AT)
        for height, block_hash in ((H - 1, h1_hash), (H, h_hash)):
            coinstake = staker.getblock(block_hash, 2)["tx"][1]
            assert_equal(self.coinstake_reward(staker, coinstake), POS_REWARD)

        # ---- the switch of the stake modifier falls at H, and nowhere else -------------------------------
        # checked as soon as block H exists, so that a switch in the wrong place fails here and not 30 blocks on.
        # The chain of modifiers is unbroken from genesis, so the first proof-of-stake block is checked too when
        # getblock prints the modifier of the last proof-of-work block.
        start = LAST_POW_BLOCK if "modifier" in staker.getblock(staker.getblockhash(LAST_POW_BLOCK)) else LAST_POW_BLOCK + 1
        before, after = self.check_modifiers(staker, start, H)
        assert_equal((before, after), (H - (start + 1), 1))
        self.log.info("Up to block H every stake modifier matches the rule of its own side: %d under the old, %d under the new",
                      before, after)

        self.log.info("Staking on, to H + %d", AFTER)
        self.stake_exactly_to(staker, H + AFTER)
        self.mark("staked to H + %d" % AFTER)
        before, after = self.check_modifiers(staker, start, H + AFTER)
        assert_equal((before, after), (H - (start + 1), AFTER + 1))
        self.log.info("All of them: %d block(s) under the old rule, %d under the new", before, after)

        self.log.info("The peer took every block over P2P and holds the same chain")
        assert_equal(peer.getbestblockhash(), staker.getbestblockhash())
        expected = self.snapshot(staker)
        self.assert_same(peer, expected, "peer")

        # ---- back across H and forward again -------------------------------------------------------------
        self.log.info("invalidateblock at H - 2, then reconsiderblock: the tip goes back across H and returns")
        hash_h2 = staker.getblockhash(H - 2)
        hash_h3 = staker.getblockhash(H - 3)
        staker.invalidateblock(hash_h2)
        assert_equal(staker.getbestblockhash(), hash_h3)
        staker.reconsiderblock(hash_h2)
        self.assert_same(staker, expected, "after reconsiderblock")
        self.check_modifiers(staker, H - MODIFIER_WINDOW, H + AFTER)

        # ---- restart, then -reindex ----------------------------------------------------------------------
        # A node that starts up hands itself the mock clock it holds for it, after any argument given here
        set_node_times(self.nodes, self.mocktime)
        self.log.info("Restart")
        self.restart_node(0, extra_args=self.extra_args[0])
        self.assert_same(staker, expected, "after a restart")
        self.mark("restarted")

        self.log.info("Reindex re-validates every block from the block files, the connect-time modifiers included")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: staker.getblockcount() == expected["height"], timeout=600)
        self.assert_same(staker, expected, "after -reindex")
        self.check_modifiers(staker, start, H + AFTER)
        self.mark("reindexed")

        # ---- a node that has never seen the chain --------------------------------------------------------
        self.log.info("A node that has never seen the chain syncs it from genesis")
        self.connect_nodes(2, 0)
        self.sync_blocks([staker, late], timeout=SYNC_TIMEOUT)
        self.assert_same(late, expected, "late node")
        self.mark("late node synced")

        self.log.info("RECORD H=%d before=%d after=%d fee_before=%s fee_at=%s supply_step_before=%s supply_step_at=%s wall=%ds",
                      H, before, after, FEE_BEFORE, FEE_AT, supply_h1 - supply_h2, supply_h - supply_h1,
                      time.time() - self.started)


if __name__ == "__main__":
    V23JointActivationPosTest().main()
