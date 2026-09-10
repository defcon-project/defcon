#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""VerifyDB at level 4 on a proof-of-stake chain, and the restart it gates.

CVerifyDB::VerifyDB at level 4 disconnects the last blocks into a private coins
view and connects them again against that view. CheckProofOfStake used to read
the kernel input from the chain tip's coins instead of the view it was handed,
and at the tip every proof-of-stake block's kernel is already spent -- by that
block's own coinstake. The first reconnect therefore failed with
prevout-not-found, verifychain answered false at level 4 on every
proof-of-stake chain, and because init forces checklevel 4 whenever
-addressindex, -spentindex or -timestampindex is on, a node carrying one of
those indexes refused to start after any ordinary restart ("Corrupted block
database detected") until it was reindexed -- after which the next restart
failed the same way.

This test stakes real proof-of-stake blocks on regtest, the way
feature_pos_staking.py does, on a node that carries -addressindex from its
first block, and then holds the node to both consequences:

1. verifychain at level 3 is true (the control: the disconnect side is sound,
   so a failure at level 4 is attributable to the reconnect step and nothing
   earlier), and verifychain at level 4 over the whole chain is true;
2. a plain restart -- no -reindex -- brings the node back to the same tip,
   through the level-4 verification that init imposes on an indexed node.
"""

import time

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

# mock-clock discipline, as in feature_pos_staking.py: bump the clock per chunk
# so block times never run ahead of it, and stay inside the block download
# timeout (one block interval, also in mock time)
POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100

STAKED_BLOCKS = 2
STAKE_CLOCK_STEP = 64
STAKE_TIMEOUT = 300


class PosVerifyChainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # from genesis: an additional index cannot be switched on over the
        # framework's cached chain without a reindex, and the chain is mined
        # to the proof-of-stake boundary here anyway
        self.setup_clean_chain = True
        # -addressindex is what makes init hold the node to checklevel 4 on
        # every start; -txindex is not needed for that but matches the shape
        # of an explorer-class node, which is the node this defect bit
        self.extra_args = [["-staking=1", "-txindex=1", "-addressindex=1", "-debug=pos"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def staking_info(self, node):
        return node.getstakinginfo()[str(self.wallet_id)]

    def stake_block_at(self, node, height, timeout=STAKE_TIMEOUT):
        deadline = time.time() + timeout * self.options.timeout_factor
        while time.time() < deadline:
            if node.getblockcount() >= height:
                return node.getblockhash(height)
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        raise AssertionError("no block staked at height %d within %ds; getstakinginfo: %s"
                             % (height, timeout, self.staking_info(node)))

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

    def run_test(self):
        node = self.nodes[0]
        if self.mocktime == 0:
            self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining proof-of-work blocks up to lastPowBlock (%d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)

        self.log.info("Staking %d proof-of-stake blocks", STAKED_BLOCKS)
        force_finish_mnsync(node)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        self.wallet_id = int(next(iter(wallets)))
        assert_equal(node.setstaking(self.wallet_id), True)
        self.wait_until(lambda: self.staking_info(node)["minter_running"], timeout=30)
        for i in range(STAKED_BLOCKS):
            height = LAST_POW_BLOCK + 1 + i
            blockhash = self.stake_block_at(node, height)
            assert_equal(node.getblock(blockhash)["flags"], "proof-of-stake")
        assert_equal(node.setstaking(self.wallet_id), False)
        final_height = self.settled_height(node)
        final_hash = node.getbestblockhash()
        assert_greater_than(final_height, LAST_POW_BLOCK)
        self.log.info("%d proof-of-stake block(s) on the chain, tip %d", final_height - LAST_POW_BLOCK, final_height)

        self.log.info("Level 3 is the control: the disconnect side is sound")
        assert_equal(node.verifychain(3, 0), True)

        self.log.info("Level 4 reconnects every block against the verifier's own view")
        # the whole chain, so the reconnect crosses from the proof-of-work era
        # into the proof-of-stake one and validates every staked block
        assert_equal(node.verifychain(4, 0), True)
        # and the last few blocks alone -- the depth init uses at startup
        assert_equal(node.verifychain(4, 6), True)

        self.log.info("A plain restart of an indexed node must come back to the same tip")
        # init forces checklevel 4 for -addressindex (parameter interaction);
        # this used to fail with "Corrupted block database detected" until
        # the node was reindexed, and again at the next restart after that
        self.restart_node(0)
        assert_equal(node.getblockcount(), final_height)
        assert_equal(node.getbestblockhash(), final_hash)
        assert_equal(node.getblock(final_hash)["flags"], "proof-of-stake")
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            log = f.read()
        assert "additional indexes -> setting -checklevel=4" in log, \
            "the parameter interaction this test relies on did not fire"
        assert "found unconnectable block" not in log
        assert "Corrupted block database" not in log


if __name__ == "__main__":
    PosVerifyChainTest().main()
