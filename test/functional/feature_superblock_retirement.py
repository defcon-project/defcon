#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The retirement of superblocks, seen through getblocktemplate.

A test chain, the superblock spork switched on, and the retirement height set
with -testactivationheight=superblocksretired@N: the one state no unit test
reaches, because a fresh spork manager on a test chain holds the spork's
default.

getblocktemplate asks whether superblocks are enabled twice, both times for
the block it is about to describe: once before it builds the template (with
the spork on, a node whose masternode sync has not finished refuses to build a
template for a superblock height), and once for the superblocks_enabled field
of the answer. Regtest's superblock heights are 1500 and every 20th block
after it, so the retirement is put on 1500 itself:

  tip 1498, next 1499, ordinary, below the retirement:  superblocks_enabled true
  tip 1499, next 1500, a superblock height, retired:    a template, and false
  tip 1500, next 1501:                                  false

The alias of -testactivationheight is what sets the height, so the test holds
the alias as well.
"""
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import append_config, assert_equal

RETIRED_AT = 1500
SPORK_KEY = "cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"


class SuperblockRetirementTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2  # getblocktemplate refuses a node with no peer
        self.setup_clean_chain = True
        self.extra_args = [["-testactivationheight=superblocksretired@%d" % RETIRED_AT]] * 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_nodes(self, num_nodes, extra_args=None, **kwargs):
        super().add_nodes(num_nodes, extra_args, **kwargs)
        append_config(self.nodes[0].datadir, ["sporkkey=" + SPORK_KEY])

    def mine(self, count):
        # Block times creep ahead of a frozen mock clock, about a second every
        # six blocks, and a block more than two hours ahead is refused: move
        # the clock along with the chain.
        node = self.nodes[0]
        done = 0
        while done < count:
            step = min(250, count - done)
            self.generate(node, step, sync_fun=self.no_op)
            done += step
            self.now += 100
            for n in self.nodes:
                n.setmocktime(self.now)
        self.sync_blocks()

    def template(self):
        return self.nodes[0].getblocktemplate({"rules": ["segwit"]})

    def run_test(self):
        node = self.nodes[0]
        self.now = int(time.time())
        for n in self.nodes:
            n.setmocktime(self.now)

        self.log.info("Switch the superblock spork on, on both nodes")
        node.sporkupdate("SPORK_9_SUPERBLOCKS_ENABLED", 0)
        for n in self.nodes:
            self.wait_until(lambda n=n: n.spork("active")["SPORK_9_SUPERBLOCKS_ENABLED"])

        self.log.info("Below the retirement the spork decides")
        self.mine(RETIRED_AT - 2)
        assert_equal(node.getblockcount(), RETIRED_AT - 2)
        below = self.template()
        assert_equal(below["height"], RETIRED_AT - 1)
        assert_equal(below["superblocks_enabled"], True)

        self.log.info("The block at the retirement height: a template, and superblocks off")
        self.mine(1)
        at = self.template()
        assert_equal(at["height"], RETIRED_AT)
        assert_equal(at["superblocks_enabled"], False)
        assert_equal(at["superblock"], [])

        self.log.info("The block is mined and both nodes take it; superblocks stay off")
        self.mine(1)
        assert_equal(self.nodes[1].getblockcount(), RETIRED_AT)
        after = self.template()
        assert_equal(after["height"], RETIRED_AT + 1)
        assert_equal(after["superblocks_enabled"], False)


if __name__ == '__main__':
    SuperblockRetirementTest().main()
