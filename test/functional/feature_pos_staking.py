#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stake real proof-of-stake blocks on regtest, and prove the chain is past its
proof-of-work era while doing so.

Until this test existed no automated test reached a proof-of-stake height:
every regtest fixture stopped well below lastPowBlock (5000 on regtest), the
unit fixtures fake the boundary by lowering lastPowBlock on a copied parameter
set, and no functional test ever switched a wallet's staking on. The rules
that only apply from the first proof-of-stake block -- the nonce rule, the
strict block-time rule, the coinbase bound, the connect-time stake modifier,
the coinstake accounting of a descriptor wallet -- therefore had no
regression path through a running node. This is that path.

What it does, in order:

1. mines through lastPowBlock with ordinary proof-of-work blocks, advancing
   the mock clock so the block times never run ahead of it;
2. shows that a proof-of-work block is refused above the boundary (the
   negative control -- without it a fixture that merely reaches height 5000
   proves nothing about the regime it is in);
3. switches staking on for the wallet, drives the mock clock forward one
   search window at a time, and collects several staked blocks;
4. checks each staked block against the proof-of-stake rules that regtest
   activates at height 0 (nonce, strict time, timestamp mask, stake
   modifier, empty coinbase output plus the credit-pool share, coinstake
   shape and reward), checks the wallet books its own coinstake as a gain of
   exactly the reward, and has a second, non-staking node accept every block
   over P2P;
5. restarts the staking node with -reindex and has it rebuild the same chain
   from disk, which re-validates every proof-of-stake block from scratch.

The mock clock matters twice. A stake attempt is keyed to a search time that
must be strictly after the tip's time, so a frozen clock can stake at most
once; and a coin can only stake once its block is stakeAgeRange[0] seconds
older than the block being staked (600 s on regtest), which is block-time
arithmetic and not wall-clock time.
"""

import time
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
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
# GetProofOfStakeReward(), validation.cpp
POS_REWARD = Decimal("500")
# GetMasternodePayment() is a fixed 10 000; once MN_RR is active (height 900 on
# regtest) PlatformShare() reallocates 37.5 % of it to the credit pool as an
# OP_RETURN output of the coinbase, before the masternode list is consulted
# (masternode/payments.cpp, GetBlockTxOuts) -- so with no masternode registered
# that share is the only value a proof-of-stake coinbase carries
MASTERNODE_PAYMENT = Decimal("10000")
PLATFORM_SHARE = MASTERNODE_PAYMENT * 375 / 1000

# regtest block times creep about one second per six blocks above a frozen mock
# clock (each block is max(median-time-past + 1, mocktime)); a chunk this size
# stays well inside MAX_FUTURE_BLOCK_TIME when the clock is bumped per chunk.
# The bump is smaller than the block download timeout (one block interval,
# 150 s, also measured in mock time) and is only applied once the observer has
# caught up, so no block is in flight when the clock jumps -- a jump across an
# in-flight request has the observer drop its only peer as a staller.
POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100

STAKED_BLOCKS = 4


class PosStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # node 0 stakes and indexes transactions (the coinstake inputs are read
        # back by txid); node 1 only validates what it receives over P2P
        self.extra_args = [
            ["-staking=1", "-txindex=1", "-debug=pos"],
            ["-staking=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ----- clock -----------------------------------------------------------

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    # ----- proof-of-work era -----------------------------------------------

    def mine_to_boundary(self, node, address):
        self.log.info("Mining proof-of-work blocks up to lastPowBlock (%d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.sync_blocks)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)
        tip = node.getblock(node.getbestblockhash())
        assert_equal(tip["flags"], "proof-of-work")

    def assert_pow_refused_above_boundary(self, node, address):
        """A proof-of-work block above lastPowBlock must not be minable: the
        template carries a non-zero nonce, which the proof-of-stake header rule
        rejects, and the block-level rule behind it rejects the regime itself."""
        height_before = node.getblockcount()
        try:
            self.generatetoaddress(node, 1, address, sync_fun=self.no_op)
        except JSONRPCException as e:
            message = e.error["message"]
            assert ("bad-pos-nonce" in message) or ("pow-late" in message), \
                "unexpected refusal of a proof-of-work block above lastPowBlock: %s" % message
            self.log.info("proof-of-work block refused above lastPowBlock: %s", message)
        else:
            raise AssertionError("a proof-of-work block was accepted above lastPowBlock")
        assert_equal(node.getblockcount(), height_before)

    # ----- proof-of-stake era ----------------------------------------------

    def staking_info(self, node):
        # getstakinginfo answers per staking wallet, keyed by the wallet id
        return node.getstakinginfo()[str(self.wallet_id)]

    def enable_staking(self, node):
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        self.wallet_id = int(next(iter(wallets)))
        # setstaking is a toggle: True means the switch is now on
        assert_equal(node.setstaking(self.wallet_id), True)
        assert_equal(self.staking_info(node)["staking"], True)
        self.wait_until(lambda: self.staking_info(node)["minter_running"], timeout=30)

    def stake_one_block(self, node, timeout=180):
        """Drive the clock one search window at a time until the staking node
        connects a new block. A wallet that found nothing eligible parks its
        search for 60 s of chain time, so each step clears that as well."""
        start = node.getblockcount()
        deadline = time.time() + timeout * self.options.timeout_factor
        while time.time() < deadline:
            if node.getblockcount() > start:
                assert_equal(node.getblockcount(), start + 1)
                return node.getbestblockhash()
            self.advance_clock(64)
            time.sleep(1)
        raise AssertionError("no block staked within %ds; getstakinginfo: %s" % (timeout, self.staking_info(node)))

    def coinstake_reward(self, node, coinstake):
        value_in = Decimal(0)
        for vin in coinstake["vin"]:
            prev = node.getrawtransaction(vin["txid"], True)
            value_in += prev["vout"][vin["vout"]]["value"]
        value_out = sum(out["value"] for out in coinstake["vout"])
        return value_out - value_in

    def check_pos_block(self, node, blockhash, prev_time):
        block = node.getblock(blockhash, 2)  # verbosity 2 used to abort on a coinstake (#55)
        assert_equal(block["flags"], "proof-of-stake")
        # #162: a proof-of-stake header carries nonce 0, and the index judges
        # the block by it
        assert_equal(block["nonce"], 0)
        # #165: strictly after the predecessor, not merely after the median
        assert_greater_than(block["time"], prev_time)
        # the kernel protocol: the low bits the timestamp mask names are clear
        assert_equal(block["time"] & POS_TIMESTAMP_MASK, 0)
        # #164: the connect-time stake modifier is set from the kernel
        assert int(block["modifier"], 16) != 0, "stake modifier is zero on a proof-of-stake block"
        assert_greater_than(len(block["tx"]), 1)
        coinbase, coinstake = block["tx"][0], block["tx"][1]
        # the coinbase of a proof-of-stake block pays its own output empty
        # (CreateNewBlock, SetEmpty) -- the staking reward is in the coinstake;
        # what FillBlockPayments appends after it is the masternode payment,
        # which without a registered masternode is the platform share alone,
        # burned to the credit pool. #163 bounds the whole against that.
        self.log.info("coinbase outputs: %s", [(o["value"], o["scriptPubKey"]["type"]) for o in coinbase["vout"]])
        assert_equal(coinbase["vout"][0]["value"], 0)
        assert_equal(coinbase["vout"][0]["scriptPubKey"]["hex"], "")
        payments = coinbase["vout"][1:]
        assert_equal(len(payments), 1)
        assert_equal(payments[0]["scriptPubKey"]["type"], "nulldata")
        assert_equal(payments[0]["value"], PLATFORM_SHARE)
        # the coinstake marker output, then a pay-to-pubkey output the block
        # signature is checked against
        assert_equal(coinstake["vout"][0]["value"], 0)
        assert_equal(coinstake["vout"][0]["scriptPubKey"]["hex"], "")
        assert_equal(coinstake["vout"][1]["scriptPubKey"]["type"], "pubkey")
        assert_equal(self.coinstake_reward(node, coinstake), POS_REWARD)
        return block

    def wallet_total(self, node):
        mine = node.getbalances()["mine"]
        return mine["trusted"] + mine["untrusted_pending"] + mine["immature"]

    # ----- the test --------------------------------------------------------

    def run_test(self):
        staker, observer = self.nodes[0], self.nodes[1]
        if self.mocktime == 0:
            self.mocktime = staker.getblockheader(staker.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        address = staker.getnewaddress()

        self.mine_to_boundary(staker, address)
        self.assert_pow_refused_above_boundary(staker, address)

        # the masternode sync gate in the minter, and the coin-age rule: the
        # clock moves past the tip by more than the minimum stake age
        for node in self.nodes:
            force_finish_mnsync(node)
        tip_time = staker.getblockheader(staker.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + 64
        set_node_times(self.nodes, self.mocktime)

        total_before = self.wallet_total(staker)
        assert_greater_than(total_before, 0)
        self.enable_staking(staker)

        self.log.info("Staking %d blocks", STAKED_BLOCKS)
        prev_time = tip_time
        for i in range(STAKED_BLOCKS):
            blockhash = self.stake_one_block(staker)
            block = self.check_pos_block(staker, blockhash, prev_time)
            assert_equal(block["height"], LAST_POW_BLOCK + 1 + i)
            self.log.info("staked block %d at time %d, reward %s", block["height"], block["time"], POS_REWARD)
            prev_time = block["time"]
            # the wallet books its own coinstake: principal returns, reward is a gain
            assert_equal(self.wallet_total(staker) - total_before, POS_REWARD * (i + 1))

        self.log.info("The non-staking node accepts the staked blocks over P2P")
        self.sync_blocks()
        final_height = LAST_POW_BLOCK + STAKED_BLOCKS
        final_hash = staker.getbestblockhash()
        assert_equal(observer.getblockcount(), final_height)
        assert_equal(observer.getbestblockhash(), final_hash)
        assert_equal(observer.getblock(final_hash)["flags"], "proof-of-stake")
        # the proof-of-work refusal holds at the new tip as well
        self.assert_pow_refused_above_boundary(observer, address)

        self.log.info("Reindexing the staking node re-validates every proof-of-stake block from disk")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: staker.getblockcount() == final_height, timeout=600)
        assert_equal(staker.getbestblockhash(), final_hash)
        assert_equal(staker.getblock(final_hash)["flags"], "proof-of-stake")
        self.connect_nodes(0, 1)
        self.sync_blocks()


if __name__ == "__main__":
    PosStakingTest().main()
