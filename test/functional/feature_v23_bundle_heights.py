#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""-testactivationheight=v23@H on a running chain: three heights, one argument.

The v23 bundle writes every one of its heights from a single number, and
chainparams_v23_bundle_tests holds that at the level of the parameters. Each
height also has a test of its own, under its own regtest name. What no test did
was run a chain under the bundle's own name and watch different heights of it
take effect where the one number puts them. This does, for the three that can
be seen from a proof-of-work chain with no masternodes on it:

  H - 120   llmq_defcon joins the enabled quorum types (quorum list). The lead
            is counted back from the ChainLock switchover height, so this is
            the bundle's nChainLocksV2ActivationHeight at work.
  H         superblocks are retired: getblocktemplate answers
            superblocks_enabled true for H - 1 and false for H, with the
            superblock spork on. H is a superblock height on regtest (1500 and
            every 20th block), so the template for it is the retired branch.
  H + 576   the Sentinel layer's start, which the bundle derives from H rather
            than takes from the operator: dslstatus reports the height from the
            first block on, reports it reached at H + 576 and not a block
            earlier, and reports the enforcement height as unreachable -- the
            bundle leaves that one unset on purpose.

H is 1560: a multiple of the 24-block Q60 DKG interval, as the bundle requires,
with H + 576 on the Sentinel epoch grid, and below regtest's last proof-of-work
block, because getblocktemplate builds a proof-of-work template.

The control is a node restarted on the same chain with
-testactivationheight=superblocksretired@H alone: it holds the same tip, and
shows neither the quorum type nor the Sentinel height. So the first and the
third observation come from the bundle's name and not from regtest's defaults.
feature_superblock_retirement.py keeps the single name's own test; this one
does not replace it.
"""
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import append_config, assert_equal

H = 1560
FORMATION_LEAD = 120        # (signingActiveQuorumCount + 1) * dkgInterval
DSL_OFFSET = 24 * 24        # V23_DSL_ACTIVATION_OFFSET
DSL_EPOCH = 24
UNREACHABLE = 2147483647
SPORK_KEY = "cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"

BUNDLE = ["-testactivationheight=v23@%d" % H]
SINGLE = ["-testactivationheight=superblocksretired@%d" % H]


class V23BundleHeightsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2  # getblocktemplate refuses a node with no peer
        self.setup_clean_chain = True
        self.extra_args = [BUNDLE, BUNDLE]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_nodes(self, num_nodes, extra_args=None, **kwargs):
        super().add_nodes(num_nodes, extra_args, **kwargs)
        append_config(self.nodes[0].datadir, ["sporkkey=" + SPORK_KEY])

    def mine_to(self, height):
        # Block times creep ahead of a frozen mock clock, about a second every
        # six blocks, and a block more than MAX_FUTURE_BLOCK_TIME (three
        # minutes here) ahead is refused: move the clock along with the chain,
        # and let the other node catch up while the clock stands still.
        node = self.nodes[0]
        while node.getblockcount() < height:
            step = min(250, height - node.getblockcount())
            self.generate(node, step, sync_fun=self.no_op)
            self.now += 100
            self.sync_blocks(wait=0.1)
            for n in self.nodes:
                n.setmocktime(self.now)
        assert_equal(node.getblockcount(), height)

    def quorum_types(self, node):
        return set(node.quorum("list").keys())

    def template(self):
        return self.nodes[0].getblocktemplate({"rules": ["segwit"]})

    def run_test(self):
        node = self.nodes[0]
        self.now = int(time.time())
        for n in self.nodes:
            n.setmocktime(self.now)

        assert_equal(H % 24, 0)
        assert_equal((H + DSL_OFFSET) % DSL_EPOCH, 0)
        assert_equal(H % 20, 0)  # a regtest superblock height: 1500 and every 20th block

        self.log.info("From the first block on the node holds the derived Sentinel height, and no enforcement height")
        for n in self.nodes:
            dsl = n.dslstatus()
            assert_equal(dsl["activationheight"], H + DSL_OFFSET)
            assert_equal(dsl["enforcementheight"], UNREACHABLE)
            assert_equal(dsl["active"], False)
            assert_equal(dsl["enforcing"], False)

        node.sporkupdate("SPORK_9_SUPERBLOCKS_ENABLED", 0)
        for n in self.nodes:
            self.wait_until(lambda n=n: n.spork("active")["SPORK_9_SUPERBLOCKS_ENABLED"])

        self.log.info("H - 120: llmq_defcon joins the enabled quorum types, and not a block earlier")
        # The enabled types are asked for the block after the tip.
        self.mine_to(H - FORMATION_LEAD - 2)
        for n in self.nodes:
            assert "llmq_defcon" not in self.quorum_types(n)
        self.mine_to(H - FORMATION_LEAD - 1)
        for n in self.nodes:
            assert "llmq_defcon" in self.quorum_types(n)

        self.log.info("H: superblocks are retired")
        self.mine_to(H - 2)
        below = self.template()
        assert_equal(below["height"], H - 1)
        assert_equal(below["superblocks_enabled"], True)
        self.mine_to(H - 1)
        # getblocktemplate asks whether superblocks are enabled before it builds
        # a template too, and that question only matters on a node whose
        # masternode sync has not finished. Put the node there.
        node.mnsync("reset")
        assert_equal(node.mnsync("status")["IsSynced"], False)
        at = self.template()
        assert_equal(at["height"], H)
        assert_equal(at["superblocks_enabled"], False)
        assert "llmq_defcon" in self.quorum_types(node)
        assert_equal(node.dslstatus()["active"], False)

        self.log.info("H + 576: the Sentinel layer's start is reached, and not a block earlier")
        self.mine_to(H + DSL_OFFSET - 1)
        for n in self.nodes:
            assert_equal(n.dslstatus()["active"], False)
        self.mine_to(H + DSL_OFFSET)
        for n in self.nodes:
            dsl = n.dslstatus()
            assert_equal(dsl["active"], True)
            assert_equal(dsl["enforcing"], False)

        self.log.info("The chain goes on past the first epoch boundary with nobody there to attest")
        self.mine_to(H + DSL_OFFSET + DSL_EPOCH + 1)
        tip = node.getbestblockhash()
        assert_equal(self.nodes[1].getbestblockhash(), tip)
        assert_equal(self.template()["superblocks_enabled"], False)

        self.log.info("Control: the single name on the same chain shows neither of the other two")
        # The framework starts a node on the mock time it holds for that node,
        # and appends it after any argument given here: hand it this test's clock,
        # or the node comes up years behind its own chain and refuses to load it.
        self.nodes[1].mocktime = self.now
        self.restart_node(1, extra_args=SINGLE)
        single = self.nodes[1]
        assert_equal(single.getbestblockhash(), tip)
        assert "llmq_defcon" not in self.quorum_types(single)
        dsl = single.dslstatus()
        assert_equal(dsl["activationheight"], UNREACHABLE)
        assert_equal(dsl["active"], False)

        self.log.info("And back under the bundle's name it shows them again")
        self.restart_node(1, extra_args=BUNDLE)
        assert_equal(self.nodes[1].getbestblockhash(), tip)
        assert "llmq_defcon" in self.quorum_types(self.nodes[1])
        assert_equal(self.nodes[1].dslstatus()["activationheight"], H + DSL_OFFSET)


if __name__ == '__main__':
    V23BundleHeightsTest().main()
