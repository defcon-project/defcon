#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The address index's mature/immature split on a proof-of-stake chain.

Consensus holds back both kinds of generated output under COINBASE_MATURITY
(consensus/tx_verify.cpp: `(coin.IsCoinBase() || coin.IsCoinStake())`), and this
chain has two: the coinbase at position 0 of a block, and the coinstake at
position 1 of a proof-of-stake block. getaddressbalance used to ask only about
position 0, so a freshly staked coinstake -- the returned principal as well as
the reward -- was reported as spendable while the node still refused to spend it
with bad-txns-premature-spend-of-coinbase.

The test stakes one block on an indexed node and then holds the RPC to two
independent statements: the immature column must equal what the maturity rule
says about the raw index entries, and the node must indeed refuse to spend the
coinstake output the column is about.
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
# consensus/consensus.h
COINBASE_MATURITY = 25

POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
STAKE_CLOCK_STEP = 64
STAKE_TIMEOUT = 300


class AddressIndexCoinstakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # -addressindex refuses to attach to a chainstate built without it
        self.setup_clean_chain = True
        self.extra_args = [["-staking=1", "-txindex=1", "-addressindex=1",
                            "-spentindex=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def balance(self, address):
        return self.nodes[0].getaddressbalance({"addresses": [address]})

    def immature_by_the_rule(self, address, tip_height, count_coinstakes):
        """What the maturity rule says about the raw index entries.

        A received entry (positive delta) is a generated output if it sits at
        position 0 of its block, or -- when count_coinstakes is set -- at
        position 1 above lastPowBlock, where every block is proof-of-stake.
        """
        total = 0
        for delta in self.nodes[0].getaddressdeltas({"addresses": [address]}):
            if delta["satoshis"] <= 0:
                continue
            generated = delta["blockindex"] == 0 or (
                count_coinstakes and delta["blockindex"] == 1
                and delta["height"] > LAST_POW_BLOCK)
            if generated and tip_height - delta["height"] < COINBASE_MATURITY:
                total += delta["satoshis"]
        return total

    def stake_one_block(self, node, height):
        deadline = time.time() + STAKE_TIMEOUT * self.options.timeout_factor
        while time.time() < deadline:
            if node.getblockcount() >= height:
                return
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        raise AssertionError("no block staked at height %d within %ds"
                             % (height, STAKE_TIMEOUT))

    def settled_height(self, node, quiet=4, timeout=60):
        """The block count once it has stopped moving: with staking switched off
        at most one more attempt can land."""
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

    def run_test(self):
        node = self.nodes[0]
        if self.mocktime == 0:
            self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining to lastPowBlock (%d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)

        # the coinbase half of the rule works, and the address has something in
        # the maturity window: without this the test could pass on a broken
        # column that is simply always zero
        before = self.balance(address)
        assert_greater_than(before["balance_immature"], 0)

        force_finish_mnsync(node)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)

        self.log.info("Staking a block")
        wallet_id = int(next(iter(node.liststakingwallets())))
        assert_equal(node.setstaking(wallet_id), True)
        self.wait_until(lambda: node.getstakinginfo()[str(wallet_id)]["minter_running"], timeout=30)
        self.stake_one_block(node, LAST_POW_BLOCK + 1)
        assert_equal(node.setstaking(wallet_id), False)
        tip_height = self.settled_height(node)
        assert_greater_than(tip_height, LAST_POW_BLOCK)

        block = node.getblock(node.getblockhash(LAST_POW_BLOCK + 1), 2)
        assert_equal(block["flags"], "proof-of-stake")
        coinstake = block["tx"][1]
        coinstake_out = sum(out["value"] for out in coinstake["vout"])
        assert_greater_than(coinstake_out, 0)
        self.log.info("coinstake %s pays %s", coinstake["txid"], coinstake_out)

        after = self.balance(address)
        expected = self.immature_by_the_rule(address, tip_height, count_coinstakes=True)
        coinbase_only = self.immature_by_the_rule(address, tip_height, count_coinstakes=False)

        # the column must follow the rule the node itself applies
        assert_equal(after["balance_immature"], expected)
        # and the difference the coinstakes make is not zero: this is what the
        # coinbase-only test used to miss
        assert_greater_than(expected - coinbase_only, 0)
        assert_equal(after["balance"], after["balance_immature"] + after["balance_spendable"])

        self.log.info("And the node refuses to spend what the column calls immature")
        depth = node.getblockcount() - (LAST_POW_BLOCK + 1) + 1
        assert_greater_than(COINBASE_MATURITY, depth)
        pay_to_pubkey = coinstake["vout"][1]
        raw = node.createrawtransaction(
            [{"txid": coinstake["txid"], "vout": 1}],
            {address: pay_to_pubkey["value"] - Decimal("0.001")})
        result = node.testmempoolaccept([raw])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], "bad-txns-premature-spend-of-coinbase")


if __name__ == "__main__":
    AddressIndexCoinstakeTest().main()
