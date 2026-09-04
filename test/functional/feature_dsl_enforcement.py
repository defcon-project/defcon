#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The Sentinel Layer's punishing branch, end to end on regtest.

feature_dsl_service.py covers the shadow half: announcements flood, reports
pool, a commitment is mined, and the verdict is recorded without acting on
anyone. This covers the other half. Enforcement had no switch on any network --
nDSLEnforcementHeight was assigned nowhere in the source -- so the branch that
suspends rewards and bans had never executed anywhere, on any chain, including
regtest. With -testactivationheight=dslenforcement it can be reached, and this
test drives one masternode all the way through it: a missed epoch counter that
climbs, reward suspension at nDSLSuspendEpochs, a service ban at nDSLBanEpochs,
and nobody else touched.

The revive at the end is the part worth having a test for. A service-banned
masternode is not valid, so the active manager holds its identity back, and it
can only announce itself because the service layer resolves its own identity
from the operator key. Without that, a service ban would be permanent: its only
exit is a later observation that the node is online, and a node that cannot
speak can never produce one.
"""

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
CUTOFF = EPOCH_INTERVAL - EPOCH_INTERVAL // 4  # reports are emitted from this offset
SUSPEND_EPOCHS = 4  # consensus.nDSLSuspendEpochs
BAN_EPOCHS = 5      # consensus.nDSLBanEpochs


class DSLEnforcementTest(DashTestFramework):
    def set_test_params(self):
        # Seven masternodes: a stopped one is 14.3% of the network, just under
        # the 15% mass-outage guard, so its missed epochs actually count
        # instead of the guard freezing every epoch. Six survivors also leave
        # enough sentinels for the aggregation threshold (five must agree).
        self.extra_args = [[
            "-testactivationheight=dsl@1",
            "-testactivationheight=dslenforcement@1",
        ]] * 8
        self.set_dash_test_params(8, 7, extra_args=self.extra_args)

    def alive(self, stopped_idx=None):
        return [n for i, n in enumerate(self.nodes) if i != stopped_idx]

    def phase_epoch(self, expect_responded, stopped_idx=None):
        """Walk one whole epoch at the pace the flood needs, and return the
        service commitments mined at its boundary.

        A block burst straight to the cutoff would give the announcements no
        time to spread and make everyone look missed, which is the mass-outage
        guard's business and not this test's. So: enter the epoch, wait for the
        announcements, walk to the cutoff so the reports pool, pause at the
        signing offset long enough for the quorum's threshold signature to
        recover, then cross the boundary where the commitment is mined.
        """
        node = self.nodes[0]
        alive = self.alive(stopped_idx)
        height = node.getblockcount()
        self.bump_mocktime(60, nodes=alive)
        self.generate(node, EPOCH_INTERVAL - (height % EPOCH_INTERVAL), sync_fun=lambda: self.sync_blocks(alive))
        self.wait_until(lambda: node.dslstatus()["respondedcount"] == expect_responded, timeout=60)
        self.bump_mocktime(30, nodes=alive)
        self.generate(node, CUTOFF, sync_fun=lambda: self.sync_blocks(alive))
        self.wait_until(lambda: node.dslstatus()["epochreports"] > 0, timeout=60)
        self.generate(node, EPOCH_INTERVAL - CUTOFF - 2, sync_fun=lambda: self.sync_blocks(alive))
        self.bump_mocktime(10, nodes=alive)
        time.sleep(3)
        self.generate(node, 2, sync_fun=lambda: self.sync_blocks(alive))
        block = node.getblock(node.getbestblockhash(), 2)
        commitments = [tx for tx in block["tx"] if tx.get("type") == 10]
        # A block carries at most one, and since a commitment may only sit at
        # the boundary that closes its own epoch, that is one per epoch. Two
        # would each be applied in turn.
        assert len(commitments) <= 1, "a block carried more than one service commitment"
        return commitments

    def service_state(self, protx_hash):
        return self.nodes[0].protx("info", protx_hash)["state"]

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        self.log.info("Forming the quorums that attest the commitments")
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        # Two of them, and llmq_test keeps two active for signing, so an epoch's
        # attesting quorum is a real choice between them rather than the only
        # one there is. The block rule re-derives that choice and refuses a
        # commitment from the other quorum, so with a single quorum in the set
        # the binding would be satisfied by anything and this test would not be
        # measuring it.
        self.mine_quorum()
        self.mine_quorum()
        assert_equal(len(node.quorum("list")["llmq_test"]), 2)

        target = self.mninfo[0]
        target_protx = target.proTxHash
        others = [mn.proTxHash for mn in self.mninfo[1:]]

        self.log.info("Stopping one masternode and letting the verdict accumulate")
        self.stop_node(target.nodeIdx)

        suspended_at = None
        banned_at = None
        epochs_walked = 0
        for _ in range(14):
            epochs_walked += 1
            self.phase_epoch(expect_responded=len(self.mninfo) - 1, stopped_idx=target.nodeIdx)
            state = self.service_state(target_protx)
            if suspended_at is None and state["rewardSuspended"]:
                suspended_at = state["missedServiceEpochs"]
            if state["dslBanHeight"] != -1:
                banned_at = state["missedServiceEpochs"]
                break

        assert banned_at is not None, "the service ban never landed"
        self.log.info(f"Suspended after {suspended_at} missed epochs, banned after {banned_at}"
                      f" ({epochs_walked} epochs walked)")
        assert_equal(suspended_at, SUSPEND_EPOCHS)
        assert_equal(banned_at, BAN_EPOCHS)
        # The counter only advances on an epoch that actually committed, so the
        # budget is what says the commitments are landing every time. A signer
        # that picked its quorum by a different rule than the block re-derives
        # would have half its commitments refused and need roughly twice as
        # many epochs to get here.
        assert epochs_walked <= BAN_EPOCHS + 1, f"the ban took {epochs_walked} epochs, expected {BAN_EPOCHS}"
        assert self.service_state(target_protx)["dslBanHeight"] > 0

        self.log.info("The other six are untouched -- no counter, no suspension, no ban")
        for other in others:
            state = self.service_state(other)
            assert_equal(state["missedServiceEpochs"], 0)
            assert_equal(state["rewardSuspended"], False)
            assert_equal(state["dslBanHeight"], -1)

        self.log.info("A service ban is not a DKG-PoSe ban: that domain is untouched")
        assert_equal(self.service_state(target_protx)["PoSeBanHeight"], -1)

        self.log.info("Restarting it: a service-banned masternode can still answer for itself")
        self.start_masternode(target)
        # the restarted node carries the mocktime it was stopped at, and headers
        # arriving before its clock catches up read as time-too-new, which costs
        # it its only peer. Clock first, connection second.
        self.bump_mocktime(1)
        self.connect_nodes(target.nodeIdx, 0)
        self.sync_blocks()

        state = None
        for _ in range(8):
            self.phase_epoch(expect_responded=len(self.mninfo))
            state = self.service_state(target_protx)
            if state["dslBanHeight"] == -1:
                break

        self.log.info("One online observation clears the ban, the suspension and the counter")
        assert_equal(state["dslBanHeight"], -1)
        assert_equal(state["rewardSuspended"], False)
        assert_equal(state["missedServiceEpochs"], 0)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLEnforcementTest().main()
