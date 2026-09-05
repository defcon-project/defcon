#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""DSL commitment and convergence scenarios under injected faults, on regtest.

Seven masternodes, DSL active from genesis, an attesting quorum mined, fault
injection enabled everywhere. Each scenario carries its own scenario id on the
faults it arms, and the assertions read the chain (the commitment mined at the
epoch boundary, resolved bit by bit to masternodes), the masternode state
(`missedServiceEpochs`) and the convergence telemetry (`dslstatus` pool hash
and candidate verdict) -- never the fault list alone.

  missed-1-2-3      a running masternode held silent for three epochs is
                    committed MISSED three times, its counter reads 1, 2, 3,
                    nobody else is ever marked, and one clean epoch resets it.
  diverged-pool     two sentinels deliver their reports after the signing
                    offset: the quorum signs a verdict the miner's later pool
                    no longer reproduces, so the boundary carries no
                    commitment and no counter moves -- fail-open.
  quorum-member-skip one of three signing members withholding its share still
                    yields a commitment; two withholding yield none.
  miner-skip        the block producer leaves the commitment out; the epoch
                    closes without one, and the next epoch commits again.

Shadow mode throughout: counters move, penalties never do.

Timing rule the whole file obeys: a masternode announces its liveness at the
tick of the boundary block that opens an epoch, so a fault meant for an epoch
must be armed *before* that boundary is mined -- between `walk` and `close`.
"""

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync

EPOCH = 24
CUTOFF = EPOCH - EPOCH // 4   # 18: reports are emitted from here
SIGNING = EPOCH - EPOCH // 8  # 21: the quorum is asked to sign from here
ARGS = ["-testactivationheight=dsl@1", "-enablefaultinjection=1"]
DSL_TX_TYPE = 10


def canonical(hashes):
    """The order ApplyServiceCommitment resolves bits by: uint256 memcmp, i.e. by internal (reversed) bytes."""
    return sorted(hashes, key=lambda h: bytes.fromhex(h)[::-1])


class DSLScenariosTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(8, 7, extra_args=[ARGS] * 8)

    # -- chain helpers -------------------------------------------------------

    def commitment_in(self, node, height):
        block = node.getblock(node.getblockhash(height), 2)
        txs = [tx for tx in block["tx"] if tx.get("type") == DSL_TX_TYPE]
        assert len(txs) <= 1, "one commitment per block at most"
        return txs[0]["poseServiceTx"]["commitment"] if txs else None

    def missed_protx(self, node, commitment):
        base_height = commitment["epoch"] * EPOCH
        assert_equal(node.getblockhash(base_height), commitment["epochBlockHash"])
        order = canonical([m["proRegTxHash"] for m in node.protx("diff", 1, base_height)["mnList"]])
        assert_equal(len(order), commitment["size"])
        return sorted(order[i] for i in commitment["missedIndices"])

    def missed_epochs(self, node, protx):
        # Three views of one fact: the miner's live list, the miner's list at
        # the tip block, and a non-mining node's list at the same block. They
        # must agree; a miner whose live list drifts from what it persisted
        # would be a finding in its own right.
        tip = node.getbestblockhash()
        live = node.protx("info", protx)["state"]["missedServiceEpochs"]
        at_tip = node.protx("info", protx, tip)["state"]["missedServiceEpochs"]
        peer = self.nodes[1].protx("info", protx, tip)["state"]["missedServiceEpochs"]
        self.log.info("  counter %s @%d: live=%d tip=%d peer=%d", protx[:8], node.getblockcount(), live, at_tip, peer)
        assert_equal((live, at_tip), (peer, peer))
        return live

    def far(self, node):
        return node.getblockcount() + 20 * EPOCH

    def mn_node(self, protx):
        return self.nodes[[m for m in self.mninfo if m.proTxHash == protx][0].nodeIdx]

    # -- epoch phasing -------------------------------------------------------

    def align(self, node):
        """Mine to position EPOCH-1, so the next `close` opens an epoch cleanly."""
        height = node.getblockcount()
        want = EPOCH - 1
        if height % EPOCH != want:
            self.bump_mocktime(60)
            self.generate(node, (want - height % EPOCH) % EPOCH)
        assert_equal(node.getblockcount() % EPOCH, want)

    def close(self, node):
        """Mine the boundary block: it closes the current epoch (carrying its
        commitment, or nothing) and opens the next one -- whose announcements
        happen at this block's tick. Returns that commitment."""
        assert_equal(node.getblockcount() % EPOCH, EPOCH - 1)
        self.bump_mocktime(10)
        self.generate(node, 1)
        boundary = node.getblockcount()
        assert_equal(boundary % EPOCH, 0)
        return self.commitment_in(node, boundary)

    def walk(self, node, responders, late_pool_growth=False):
        """From position 0 to position EPOCH-1 of the current epoch: wait for
        the announcements, cross the cutoff, pause for the signing, and return
        what every node holds just before the boundary."""
        assert_equal(node.getblockcount() % EPOCH, 0)
        epoch = node.getblockcount() // EPOCH
        self.bump_mocktime(60)
        self.wait_until(lambda: node.dslstatus()["epoch"] == epoch, timeout=30)
        self.wait_until(lambda: node.dslstatus()["respondedcount"] == responders, timeout=60)
        self.bump_mocktime(30)
        self.generate(node, CUTOFF)
        self.wait_until(lambda: node.dslstatus()["epochreports"] > 0, timeout=90)
        self.settle(node)
        pooled = node.dslstatus()["epochreports"]
        self.bump_mocktime(10)
        self.generate(node, SIGNING - CUTOFF)
        time.sleep(4)  # the quorum's threshold signature recovers from the pool as it is now
        self.bump_mocktime(10)
        self.generate(node, 1)  # position 22: delayed reports (param 4) are emitted here
        if late_pool_growth:
            self.wait_until(lambda: node.dslstatus()["epochreports"] > pooled, timeout=60)
            self.settle(node)
        time.sleep(1)
        self.generate(node, EPOCH - SIGNING - 2)
        assert_equal(node.getblockcount() % EPOCH, EPOCH - 1)
        return [n.dslstatus() for n in self.nodes]

    def settle(self, node):
        stable_since = time.time()
        last = node.dslstatus()["epochreports"]
        deadline = time.time() + 60
        while time.time() < deadline:
            time.sleep(1)
            now = node.dslstatus()["epochreports"]
            if now != last:
                last, stable_since = now, time.time()
            elif time.time() - stable_since >= 6:
                return

    # -- the scenarios -------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.log.info("Mining the attesting quorum")
        quorum_hash = self.mine_quorum()
        assert_equal(node.quorum("list")["llmq_test"], [quorum_hash])
        members = [m["proTxHash"] for m in node.quorum("info", 100, quorum_hash)["members"]]
        assert_equal(len(members), 3)
        mn_count = len(self.mninfo)
        target = self.mninfo[0].proTxHash
        tnode = self.mn_node(target)

        self.align(node)
        self.close(node)  # opens the first phased epoch
        self.log.info("Warm-up epoch, then a control epoch that commits nobody MISSED")
        self.walk(node, mn_count)
        self.close(node)
        self.walk(node, mn_count)

        # ---- missed-1-2-3 --------------------------------------------------
        self.log.info("missed-1-2-3: three silent epochs count 1, 2, 3; nobody else is ever marked")
        fault = tnode.faultinject("set", "response-drop", self.far(node), "missed-1-2-3")
        control = self.close(node)  # closes the control epoch, opens the first faulted one
        assert control is not None, "the control epoch mined no commitment"
        assert_equal(control["missedCount"], 0)
        assert_equal(self.missed_epochs(node, target), 0)
        for expected in (1, 2, 3):
            statuses = self.walk(node, mn_count - 1)
            # convergence: every node holds the same pool and aggregates the same verdict
            assert_equal(len({s["poolhash"] for s in statuses}), 1)
            assert all(s["candidate"]["missedprotxhashes"] == [target] for s in statuses)
            # and the telemetry names the scenario behind the verdict
            active = [f for f in statuses[tnode.index]["faults"] if f["id"] == fault["id"]]
            assert_equal(active[0]["scenarioId"], "missed-1-2-3")
            assert_greater_than(active[0]["hits"], 0)
            if expected == 3:
                assert_equal(tnode.faultinject("clear")["cleared"], 1)  # the next epoch opens clean
            c = self.close(node)
            assert c is not None, "faulted epoch %d mined no commitment" % expected
            assert_equal(self.missed_protx(node, c), [target])
            assert_equal(self.missed_epochs(node, target), expected)
            for other in self.mninfo[1:]:
                assert_equal(self.missed_epochs(node, other.proTxHash), 0)
        # shadow mode: recorded, never penalised
        info = node.protx("info", target)["state"]
        assert_equal(info["rewardSuspended"], False)
        assert_equal(info["dslBanHeight"], -1)
        self.log.info("  recovery: one clean epoch resets the counter")
        self.walk(node, mn_count)

        # ---- diverged-pool -------------------------------------------------
        self.log.info("diverged-pool: reports arriving after the signing offset leave the boundary without a commitment")
        late = [self.mn_node(m.proTxHash) for m in self.mninfo[1:3]]
        tnode.faultinject("set", "response-drop", self.far(node), "diverged-pool")
        for lnode in late:
            lnode.faultinject("set", "report-delay", self.far(node), "diverged-pool", 4)
        c = self.close(node)  # closes the recovery epoch, opens the diverging one
        assert_equal(c["missedCount"], 0)
        assert_equal(self.missed_epochs(node, target), 0)
        statuses = self.walk(node, mn_count - 1, late_pool_growth=True)
        # the miner's pool, by the boundary, does say MISSED ...
        assert_equal(statuses[0]["candidate"]["missedprotxhashes"], [target])
        for lnode in late:
            assert_greater_than(statuses[lnode.index]["faults"][0]["hits"], 0)
            assert_equal(lnode.faultinject("clear")["cleared"], 1)
        assert_equal(tnode.faultinject("clear")["cleared"], 1)
        c = self.close(node)
        # ... but the quorum signed a pool without the late reports (4 < nDSLSentinelAgree), so nothing was minable
        assert c is None, "a commitment was mined from a pool that diverged from the quorum's: %r" % c
        assert_equal(self.missed_epochs(node, target), 0)
        self.walk(node, mn_count)

        # ---- quorum-member-skip --------------------------------------------
        self.log.info("quorum-member-skip: one of three withholding still commits, two do not")
        tnode.faultinject("set", "response-drop", self.far(node), "quorum-member-skip")
        # a signing member may also be the target, so its faults are cleared by id, never wholesale
        skips = [self.mn_node(members[0]).faultinject("set", "commitment-skip", self.far(node), "quorum-member-skip")]
        c = self.close(node)  # the epoch after the divergence commits again
        assert c is not None and c["missedCount"] == 0, "the epoch after the divergence did not commit"
        self.walk(node, mn_count - 1)
        c = self.close(node)
        assert c is not None, "a 2-of-3 quorum with one silent member mined no commitment"
        assert_equal(self.missed_protx(node, c), [target])
        counted = self.missed_epochs(node, target)
        assert_equal(counted, 1)
        # the second member withholds too: the signing happens during the walk, so arming now is in time
        skips.append(self.mn_node(members[1]).faultinject("set", "commitment-skip", self.far(node), "quorum-member-skip"))
        self.walk(node, mn_count - 1)
        c = self.close(node)
        assert c is None, "a quorum below its signing threshold mined a commitment"
        assert_equal(self.missed_epochs(node, target), counted)
        for protx, skip in zip(members[:2], skips):
            assert_equal(self.mn_node(protx).faultinject("clear", skip["id"])["cleared"], 1)
        self.walk(node, mn_count - 1)
        c = self.close(node)
        assert c is not None and self.missed_protx(node, c) == [target]
        assert_equal(self.missed_epochs(node, target), counted + 1)

        # ---- miner-skip ----------------------------------------------------
        self.log.info("miner-skip: the block producer leaves the commitment out, and the next epoch commits again")
        # expires exactly at the boundary: the template for it is built one height earlier, still faulted
        node.faultinject("set", "commitment-skip", node.getblockcount() + EPOCH, "miner-skip")
        counted = self.missed_epochs(node, target)
        statuses = self.walk(node, mn_count - 1)
        assert_equal(statuses[0]["faults"][0]["scenarioId"], "miner-skip")
        c = self.close(node)
        assert c is None, "the producer mined a commitment it was told to withhold"
        assert_equal(self.missed_epochs(node, target), counted)
        assert_equal(node.faultinject("list")["faults"], [])  # expired by height
        self.walk(node, mn_count - 1)
        c = self.close(node)
        assert c is not None and self.missed_protx(node, c) == [target]
        assert_equal(self.missed_epochs(node, target), counted + 1)

        assert_equal(tnode.faultinject("clear")["cleared"], 1)
        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLScenariosTest().main()
