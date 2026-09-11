#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Service-commitment format version 2, end to end: the flip, and neutrality.

A version-1 commitment carries one bitfield, `missed`. A masternode the epoch's
sentinels could not judge -- too few reports either way -- has a clear bit,
exactly like one they saw online, and applying the commitment heals it: counter
reset, suspension lifted. An epoch with thin sentinel coverage therefore cleared
everyone. Version 2 adds `observed`: a bit without a verdict changes nothing.

Two things are proved here on a live regtest with a real attesting quorum.

The flip. With -testactivationheight=dslcommitmentv2@FLIP the boundary blocks
below FLIP carry version-1 commitments and the ones from FLIP on carry version
2, on one chain, signed by the same quorums and accepted by every node. That
is what the defcon-q60 devnet will do at its own flip height, with version-1
history beneath it.

Neutrality. One masternode is stopped and observed missed for two epochs. Then
every live masternode is given a report-drop fault, so no sentinel report
reaches any pool: the quorum still signs and the miner still attaches a
commitment, but it observes nobody. That commitment must leave the stopped
node's counter exactly where it was -- not reset (the version-1 defect), not
advanced (it was not observed missed either). The faults are cleared, the next
epoch observes again, and the counter continues from where it stood. Whether a
commitment was actually mined is asserted every time, so "nothing changed"
cannot be satisfied by an absent commitment.
"""

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
CUTOFF = EPOCH_INTERVAL - EPOCH_INTERVAL // 4  # reports are emitted from this offset
DSL_ACTIVATE_AT = 1
DSL_ENFORCE_AT = 1
# The format flip. The 8-node setup with two llmq_test quorums leaves the chain
# in the 120s, so the boundaries at 144 and 168 close on version 1 and 192 is
# the first version-2 boundary. The test asserts that at least one version-1
# epoch is actually walked, so a slower setup fails loudly rather than
# silently skipping the flip.
FLIP = 192
DSL_TX_TYPE = 10


class DSLCommitmentV2Test(DashTestFramework):
    def set_test_params(self):
        # Seven masternodes: a stopped one is 14.3% of the network, just under
        # the 15% mass-outage guard, so its missed epochs actually count. Six
        # survivors leave six sentinels per target, of which five must agree.
        self.extra_args = [[
            f"-testactivationheight=dsl@{DSL_ACTIVATE_AT}",
            f"-testactivationheight=dslenforcement@{DSL_ENFORCE_AT}",
            f"-testactivationheight=dslcommitmentv2@{FLIP}",
            # the report-drop faults in the neutrality half need the injector
            "-enablefaultinjection=1",
        ]] * 8
        self.set_dash_test_params(8, 7, extra_args=self.extra_args)

    def alive(self, stopped_idx=None):
        return [n for i, n in enumerate(self.nodes) if i != stopped_idx]

    def phase_epoch(self, expect_responded, stopped_idx=None, expect_reports=True):
        """Walk one whole epoch at the pace the flood needs and return the
        commitment mined at its boundary (or None). Same choreography as
        feature_dsl_enforcement.py: enter, wait for announcements, walk to the
        cutoff so reports pool, pause at the signing offset for the threshold
        signature, cross the boundary."""
        node = self.nodes[0]
        alive = self.alive(stopped_idx)
        height = node.getblockcount()
        self.bump_mocktime(60, nodes=alive)
        self.generate(node, EPOCH_INTERVAL - (height % EPOCH_INTERVAL), sync_fun=lambda: self.sync_blocks(alive))
        self.wait_until(lambda: node.dslstatus()["respondedcount"] == expect_responded, timeout=60)
        self.bump_mocktime(30, nodes=alive)
        self.generate(node, CUTOFF, sync_fun=lambda: self.sync_blocks(alive))
        if expect_reports:
            self.wait_until(lambda: node.dslstatus()["epochreports"] > 0, timeout=60)
        else:
            # every reporter is faulted: give the reports the time they would
            # have taken, and require that none arrived
            time.sleep(3)
            assert_equal(node.dslstatus()["epochreports"], 0)
        self.generate(node, EPOCH_INTERVAL - CUTOFF - 2, sync_fun=lambda: self.sync_blocks(alive))
        self.bump_mocktime(10, nodes=alive)
        time.sleep(3)
        self.generate(node, 2, sync_fun=lambda: self.sync_blocks(alive))
        block = node.getblock(node.getbestblockhash(), 2)
        txs = [tx for tx in block["tx"] if tx.get("type") == DSL_TX_TYPE]
        assert len(txs) <= 1, "a block carried more than one service commitment"
        if not txs:
            return None
        commitment = txs[0]["poseServiceTx"]["commitment"]
        assert_equal(commitment["epoch"], block["height"] // EPOCH_INTERVAL - 1)
        commitment["_height"] = block["height"]
        return commitment

    def committed_epoch(self, **kwargs):
        """Walk epochs until one commits (a divergent pool yields none), and
        return it. Bounded: the counter only moves on a committed epoch, so a
        run that keeps missing commitments must fail here, not later."""
        for _ in range(4):
            c = self.phase_epoch(**kwargs)
            if c is not None:
                return c
        raise AssertionError("no service commitment was mined within four epochs")

    def state(self, protx_hash):
        return self.nodes[0].protx("info", protx_hash)["state"]

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        self.log.info("Every node took the flip height, read back from the daemon")
        for n in self.nodes:
            st = n.dslstatus()
            assert_equal(st["activationheight"], DSL_ACTIVATE_AT)
            assert_equal(st["enforcementheight"], DSL_ENFORCE_AT)

        self.log.info("Forming the quorums that attest the commitments")
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.mine_quorum()
        self.mine_quorum()
        assert_equal(len(node.quorum("list")["llmq_test"]), 2)
        h0 = node.getblockcount()
        self.log.info(f"Setup done at height {h0}; the format flips at {FLIP}")
        assert h0 < FLIP - EPOCH_INTERVAL, (
            f"setup reached {h0}, leaving no version-1 epoch before the flip at {FLIP}; raise FLIP")

        mn_count = len(self.mninfo)

        self.log.info("Below the flip the quorum signs and the chain accepts version 1")
        c = self.committed_epoch(expect_responded=mn_count)
        assert c["_height"] < FLIP, f"the first commitment landed at {c['_height']}, at or past the flip"
        assert_equal(c["version"], 1)
        # a version-1 commitment observes everyone, by definition of the format
        assert_equal(c["observedCount"], c["size"])
        assert_equal(c["unobservedIndices"], [])
        assert_equal(c["missedIndices"], [])
        # the candidate the pool would build says so too
        assert_equal(node.dslstatus()["candidate"]["version"], 1)
        v1_height = c["_height"]

        self.log.info("At the flip the same quorums sign version 2, and every node accepts it")
        c = self.committed_epoch(expect_responded=mn_count)
        while c["_height"] < FLIP:
            assert_equal(c["version"], 1)
            c = self.committed_epoch(expect_responded=mn_count)
        assert_equal(c["version"], 2)
        assert_equal(c["observedCount"], c["size"])
        assert_equal(c["unobservedIndices"], [])
        assert_equal(c["missedIndices"], [])
        assert_equal(node.dslstatus()["candidate"]["version"], 2)
        self.log.info(f"  version 1 at {v1_height}, version 2 at {c['_height']}; every node is at the same tip")
        self.sync_blocks()

        target = self.mninfo[0]
        target_protx = target.proTxHash
        others = [mn for mn in self.mninfo[1:]]

        self.log.info("Stopping one masternode: two observed misses")
        self.stop_node(target.nodeIdx)
        for expected in (1, 2):
            c = self.committed_epoch(expect_responded=mn_count - 1, stopped_idx=target.nodeIdx)
            assert_equal(c["version"], 2)
            assert_equal(c["missedCount"], 1)
            assert_equal(c["unobservedIndices"], [])
            assert_equal(self.state(target_protx)["missedServiceEpochs"], expected)
        last_epoch = self.state(target_protx)["lastServiceEpoch"]

        self.log.info("Every reporter drops its reports: the epoch commits, and observes nobody")
        faults = []
        until = node.getblockcount() + 3 * EPOCH_INTERVAL
        for mn in others:
            faults.append(mn.node.faultinject("set", "report-drop", until, "v2-unobserved")["id"])
        c = self.committed_epoch(expect_responded=mn_count - 1, stopped_idx=target.nodeIdx, expect_reports=False)
        assert_equal(c["version"], 2)
        assert_equal(c["missedCount"], 0)
        assert_equal(c["observedCount"], 0)
        assert_equal(sorted(c["unobservedIndices"]), list(range(c["size"])))
        st = self.state(target_protx)
        self.log.info("  the stopped node's counter is untouched: not reset, not advanced")
        assert_equal(st["missedServiceEpochs"], 2)
        assert_equal(st["rewardSuspended"], False)
        assert_equal(st["dslBanHeight"], -1)
        # but the epoch itself was recorded against it
        assert st["lastServiceEpoch"] > last_epoch, "the unobserved epoch was not recorded"
        for mn in others:
            assert_equal(self.state(mn.proTxHash)["missedServiceEpochs"], 0)

        self.log.info("Faults cleared: the streak resumes from two, not from zero")
        for mn in others:
            assert_equal(mn.node.faultinject("clear")["cleared"], 1)
        c = self.committed_epoch(expect_responded=mn_count - 1, stopped_idx=target.nodeIdx)
        assert_equal(c["missedCount"], 1)
        assert_equal(c["unobservedIndices"], [])
        assert_equal(self.state(target_protx)["missedServiceEpochs"], 3)
        for mn in others:
            assert_equal(self.state(mn.proTxHash)["missedServiceEpochs"], 0)

        self.log.info("Restarted, an observed online verdict clears it")
        self.start_masternode(target)
        self.bump_mocktime(1)
        self.connect_nodes(target.nodeIdx, 0)
        self.sync_blocks()
        st = None
        for _ in range(6):
            c = self.phase_epoch(expect_responded=mn_count)
            st = self.state(target_protx)
            if st["missedServiceEpochs"] == 0:
                break
        assert_equal(st["missedServiceEpochs"], 0)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLCommitmentV2Test().main()
