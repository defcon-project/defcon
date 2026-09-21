#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""-testactivationheight=v23@H on a masternode network: the quorum half of the bundle, on one chain.

The v23 bundle writes every one of its heights from a single number. Two tests
already stand on either side of what this one does. feature_v23_bundle_heights.py
runs a chain under the bundle's own name, but a proof-of-work chain with no
masternodes on it: it sees the heights arrive and nothing sign. And
feature_llmq_q60_switchover.py carries a real 60-member quorum through the two
switchovers, but under their own names and at two DIFFERENT heights, because
that is what tells one resolver from the other. Neither carries a masternode
network through the bundle as a release sets it, where ChainLocks and
InstantSend move onto llmq_defcon at the SAME height, and the Sentinel layer
starts 576 blocks later, attested by the quorums that same number brought into
being. This does, with H = 600:

  below 480    ChainLocks form on llmq_test, no llmq_defcon session runs, and
               the Sentinel layer reports the height it was derived
  479          the first tip whose next block enables llmq_defcon
  [480, 600)   every one of the five cycles in the formation lead forms a real
               60-member quorum, and NO ChainLock forms -- observed at H - 1,
               the last block of the pause
  600          on a tip that is not mined past, the block at H is ChainLocked
               by llmq_defcon; the lock saved from below the lead still
               verifies. On the same tip InstantSend signs on llmq_defcon; the
               transaction sent at H - 1 got no such signature
  1176         the Sentinel layer is active: every node, read back, and every
               masternode's announcement seen by every node
  1176..1200   the first epoch is WAITED OUT, not mined through: announcements,
               the DKG of the same window phase by phase, the reports and the
               verdict they add up to on every node, the threshold signature
  1200         the first service commitment: epoch 49, based on block 1176,
               format version 2, llmqType 7, nobody missed, nobody unobserved,
               signed by one of the four quorums active when the epoch began
  restart      the controller re-validates the whole chain from its own block
               files and holds the same tip, both saved ChainLocks verify, and
               the commitment reads back unchanged

Which profile signed a lock is asked of the signature, not of a flag: the same
request id, message hash and signature are offered to every quorum of both
profiles, and exactly one quorum of the expected profile accepts them.

Two economies, both deliberate. Between the switchover and the run-up to the
Sentinel start the chain is mined with the DKG spork off, the way the framework
itself crosses long spans (activate_by_name): unpaced mining cannot complete a
60-member DKG anyway, and leaving the sessions to start and abort for 450
blocks would only make the outcome depend on scheduling. The four cycles before
1176 are paced again, so the quorum that attests the first commitment is drawn
from a full, fresh active set. And regtest's v20 and mn_rr heights (900) are
pushed out of the run: no network this tree runs reaches v20, and the Sentinel
half of this chain would otherwise run under rules the release never sees.

Not in test_runner.py, for the same reason as feature_llmq_q60_dkg.py: it
raises the framework's node ceiling, and a parallel job with a different
ceiling gets a different port stride and can collide.
"""

import os
import struct
import time

# Read at import time by test_framework, so it has to be set before the import.
os.environ.setdefault("TEST_RUNNER_MAX_NODES", "160")

from test_framework.messages import CTransaction, from_hex, hash256, ser_compact_size, ser_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal, force_finish_mnsync, set_node_times

# Consensus::LLMQType, src/llmq/params.h
Q60_TYPE = 7
Q60_NAME = "llmq_defcon"
Q60_SIZE = 60
Q60_MIN_SIZE = 44
Q60_THRESHOLD = 41
Q60_DKG_INTERVAL = 24
Q60_SIGNING_ACTIVE = 4
LEGACY_TYPE = 100           # llmq_test, regtest's llmqTypeChainLocks
LEGACY_NAME = "llmq_test"

MN_COUNT = 65    # > Q60_SIZE, so membership is a real selection
MINE_CHUNK = 10  # blocks per mocktime bump

H = 600                                                        # the one number
FORMATION_LEAD = (Q60_SIGNING_ACTIVE + 1) * Q60_DKG_INTERVAL   # 120
LEAD_START = H - FORMATION_LEAD                                # 480
FIRST_ENABLED_TIP = LEAD_START - 1                             # 479: the enabled types are asked for the next block
LEAD_ROUNDS = FORMATION_LEAD // Q60_DKG_INTERVAL               # 5: every cycle of the lead

EPOCH = 24
SENTINELS = 7                   # nDSLSentinelCount: reports per masternode per epoch
CUTOFF = EPOCH - EPOCH // 4     # 18: reports are emitted from here
SIGNING = EPOCH - EPOCH // 8    # 21: the quorum is asked to sign from here
DSL_OFFSET = 24 * 24            # V23_DSL_ACTIVATION_OFFSET
DSL_START = H + DSL_OFFSET      # 1176
FIRST_EPOCH = DSL_START // EPOCH        # 49
FIRST_COMMITMENT = DSL_START + EPOCH    # 1200
DSL_TX_TYPE = 10
UNREACHABLE = 2147483647

RUNUP_ROUNDS = Q60_SIGNING_ACTIVE                              # a whole fresh active set
RUNUP_FIRST_BASE = DSL_START - RUNUP_ROUNDS * Q60_DKG_INTERVAL # 1080
NOT_IN_THIS_RUN = 4000    # v20 and mn_rr: above everything mined here

# Every wait below is bounded, and the bounds are fixed here rather than where
# they are used, so a run's record can name them. Seconds.
CL_AT_H_TIMEOUT = 90        # the lock on the block at H, on a tip that does not move
NO_Q60_ISLOCK_WAIT = 20     # how long H - 1 is watched for a signature that must not come
NO_CL_AT_PAUSE_WAIT = 10    # and, after that, for a ChainLock that must not come
ANNOUNCE_TIMEOUT = 120      # 65 announcements seen by 66 nodes
VERDICT_TIMEOUT = 180       # the reports pooled into a complete verdict on 66 nodes
SIGN_TIMEOUT_PER_TICK = 40  # the threshold signature, per block of the signing window
REINDEX_TIMEOUT = 600


def canonical(hashes):
    """The order a commitment's bits index: uint256 comparison, i.e. by internal (reversed) bytes."""
    return sorted(hashes, key=lambda h: bytes.fromhex(h)[::-1])


class V23JointActivationLLMQTest(DashTestFramework):
    def set_test_params(self):
        args = ["-testactivationheight=v23@%d" % H,
                "-testactivationheight=v20@%d" % NOT_IN_THIS_RUN,
                "-testactivationheight=mn_rr@%d" % NOT_IN_THIS_RUN]
        self.set_dash_test_params(MN_COUNT + 1, MN_COUNT, extra_args=[args] * (MN_COUNT + 1))
        # set AFTER set_dash_test_params, which resets these to the llmq_test values
        self.llmq_size = Q60_SIZE
        self.llmq_threshold = Q60_THRESHOLD

    # ---- helpers -------------------------------------------------------

    def mark(self, text):
        self.log.info("[%4d s, height %d] %s", time.time() - self.started, self.nodes[0].getblockcount(), text)

    def all_nodes(self, predicate):
        return all(predicate(n) for n in self.nodes)

    def wait_on_every_node(self, what, predicate, timeout):
        """Wait until the predicate has held on every node, and return what each node answered.

        For conditions that do not un-happen inside an epoch (an announcement
        seen, a report pooled), so a node that has answered is not asked again.
        That matters here: dslstatus aggregates the node's whole pool on every
        call, and asking 66 loaded nodes in a loop until the last one agrees
        was measured to take longer than the reports took to arrive.
        """
        pending = list(self.nodes)
        answers = {}
        deadline = time.time() + timeout * self.options.timeout_factor
        while True:
            pending = [n for n in pending if not self.answered(n, predicate, answers)]
            if not pending:
                return answers
            assert time.time() < deadline, "%s: %d node(s) not there after %d s, the first is node%d" % (
                what, len(pending), timeout, pending[0].index)
            time.sleep(1)

    @staticmethod
    def answered(n, predicate, answers):
        answer = predicate(n)
        if answer:
            answers[n.index] = answer
        return bool(answer)

    def mine_to(self, node, target, chunk=MINE_CHUNK):
        """Mine in chunks, moving the clock along with the chain.

        Block times creep ahead of a standing mock clock by about a second
        every six blocks, and a block too far ahead of the clock is refused:
        a second per five blocks keeps the clock in front.
        """
        while node.getblockcount() < target:
            step = min(chunk, target - node.getblockcount())
            self.bump_mocktime(max(1, step // 5))
            self.generate(node, step)
        assert_equal(node.getblockcount(), target)

    def wait_for_chainlock(self, node, block_hash, expected=True, timeout=90):
        """Wait on the block's own chainlock flag, bumping mocktime as we poll.

        The handler retries signing from a 5s scheduler task, and under
        mocktime that clock only moves when the test moves it. With
        expected=False the wait must time out, and the result says whether a
        lock appeared anyway.
        """
        def locked():
            self.bump_mocktime(1)
            try:
                block = node.getblock(block_hash)
                return block["confirmations"] > 0 and block["chainlock"]
            except Exception:
                return False
        return self.wait_until(locked, timeout=timeout, sleep=1, do_assert=expected)

    def best_chainlock(self, node):
        try:
            return node.getbestchainlock()
        except Exception:  # "Unable to find any ChainLock"
            return None

    def clsig_request_id(self, height):
        """SerializeHash(("clsig", int32 height)), as src/llmq/chainlocks.cpp builds it."""
        return hash256(ser_string(b"clsig") + struct.pack("<i", height))[::-1].hex()

    def signers_of(self, node, llmq_type, type_name, request_id, msg_hash, signature):
        """The quorums of one profile that accept this signature for this request.

        Asked quorum by quorum with the quorum named, so nothing here depends
        on which one a height selects: the question is only whether a quorum
        of THIS profile made the signature.
        """
        return [q for q in node.quorum("list", 100).get(type_name, [])
                if node.quorum("verify", llmq_type, request_id, msg_hash, signature, q)]

    def assert_lock_profile(self, node, lock, expect_q60):
        """The discriminating question about a ChainLock: which profile signed it.

        getbestchainlock's llmqType is resolved from the height, so it says
        what the node expects, not what the signature is. Here the signature
        itself is offered to every quorum of both profiles.
        """
        request_id = self.clsig_request_id(lock["height"])
        under_q60 = self.signers_of(node, Q60_TYPE, Q60_NAME, request_id, lock["blockhash"], lock["signature"])
        under_legacy = self.signers_of(node, LEGACY_TYPE, LEGACY_NAME, request_id, lock["blockhash"], lock["signature"])
        self.log.info("  lock at %d: accepted by %d %s and %d %s quorum(s)",
                      lock["height"], len(under_q60), Q60_NAME, len(under_legacy), LEGACY_NAME)
        assert_equal((len(under_q60), len(under_legacy)), (1, 0) if expect_q60 else (0, 1))

    def islock_request_id(self, node, txid):
        """ser_string("islock") || compact_size(len(vin)) || each prevout, double SHA256, reversed."""
        tx = from_hex(CTransaction(), node.getrawtransaction(txid))
        buf = ser_string(b"islock") + ser_compact_size(len(tx.vin))
        for txin in tx.vin:
            buf += txin.prevout.serialize()
        return hash256(buf)[::-1].hex()

    def q60_signed(self, node, txid, timeout=0):
        """Did the Q60 quorum produce a recovered signature for this txid?

        A lock flag says a lock exists; it does not say which profile signed it.
        """
        request_id = self.islock_request_id(node, txid)

        def signed():
            self.bump_mocktime(1)
            try:
                return node.quorum("hasrecsig", Q60_TYPE, request_id, txid)
            except Exception:
                return False
        if timeout == 0:
            return signed()
        return self.wait_until(signed, timeout=timeout, sleep=1, do_assert=False)

    def wait_for_internal_islock(self, node, txid, timeout=60):
        """Always instantlock_internal, never instantlock.

        instantlock is also true for a transaction in a ChainLocked block, and
        ChainLocks work here -- the plain field would report a lock that no
        ISLOCK ever produced.
        """
        def locked():
            self.bump_mocktime(1)
            try:
                return node.getrawtransaction(txid, True)["instantlock_internal"]
            except Exception:
                return False
        self.wait_until(locked, timeout=timeout, sleep=1)

    def assert_sentinel_dormant(self):
        """Not the absence of a log line: what every node says it holds, and that it holds nothing."""
        for n in self.nodes:
            dsl = n.dslstatus()
            assert_equal(dsl["activationheight"], DSL_START)
            assert_equal(dsl["enforcementheight"], UNREACHABLE)
            assert_equal((dsl["active"], dsl["enforcing"]), (False, False))
            assert_equal((dsl["respondedcount"], dsl["epochreports"]), (0, 0))

    def dsl_request_id(self, node, epoch):
        """SerializeHash of ("dslcommitment", uint32 epoch, the epoch's base block hash)."""
        base = bytes.fromhex(node.getblockhash(epoch * EPOCH))[::-1]
        return hash256(ser_string(b"dslcommitment") + struct.pack("<I", epoch) + base)[::-1].hex()

    def dsl_signature_recovered(self, node, epoch):
        # isconflicting is true only for a recovered signature with a different
        # message hash, so two distinct probes cover any hash it could have.
        self.bump_mocktime(1)
        request_id = self.dsl_request_id(node, epoch)
        return (node.quorum("isconflicting", Q60_TYPE, request_id, "00" * 32) or
                node.quorum("isconflicting", Q60_TYPE, request_id, "11" * 32))

    def commitment_at(self, node, height):
        """The service commitment a block carries, with what the quorum signed: (json, msg_hash, signature)."""
        block_hash = node.getblockhash(height)
        block = node.getblock(block_hash, 2)
        txs = [tx for tx in block["tx"] if tx.get("type") == DSL_TX_TYPE]
        assert_equal(len(txs), 1)
        raw = txs[0].get("hex") or node.getrawtransaction(txs[0]["txid"], False, block_hash)
        tx = from_hex(CTransaction(), raw)
        assert_equal(tx.nType, DSL_TX_TYPE)
        # The quorum signs the transaction with the signature zeroed, and the
        # signature is the last field of the payload's last member.
        signature = tx.vExtraPayload[-96:]
        tx.vExtraPayload = tx.vExtraPayload[:-96] + bytes(96)
        msg_hash = hash256(tx.serialize())[::-1].hex()
        assert_equal(txs[0]["poseServiceTx"]["version"], 1)
        return txs[0]["poseServiceTx"]["commitment"], msg_hash, signature.hex()

    def wait_for_quorum_connections(self, quorum_hash, expected_connections, mninfos, llmq_type_name="llmq_test", timeout=60, wait_proc=None):
        """For llmq_defcon: every member's quorum connections, not the first member's.

        The framework's version answers at the first masternode it finds with
        a session and enough connections, which is all a three-member quorum
        needs. A 60-member quorum relays its DKG messages over a sparse graph,
        and the test mines into the contribute phase the moment this returns,
        so nothing held that phase back until the members were connected. On a
        network the init phase lasts two blocks of real time; here it lasts as
        long as this wait, and the first round of a run is the one where every
        connection is new.
        """
        if llmq_type_name != Q60_NAME:
            return super().wait_for_quorum_connections(quorum_hash, expected_connections, mninfos,
                                                       llmq_type_name=llmq_type_name, timeout=timeout, wait_proc=wait_proc)

        def every_member_connected():
            for mn in mninfos:
                for qc in mn.node.quorum("dkgstatus")["quorumConnections"]:
                    if qc["llmqType"] != Q60_NAME or qc["quorumHash"] != quorum_hash:
                        continue
                    if not all(c["connected"] for c in qc.get("quorumConnections", [])):
                        if wait_proc is not None:
                            wait_proc()
                        return False
            return True
        self.wait_until(every_member_connected, timeout=timeout, sleep=1)

    def form_q60(self, node, expected_base):
        quorum_hash = self.mine_quorum(llmq_type_name=Q60_NAME, llmq_type=Q60_TYPE,
                                       expected_members=Q60_SIZE,
                                       expected_contributions=Q60_SIZE,
                                       expected_commitments=Q60_SIZE)
        assert_equal(node.quorum("info", Q60_TYPE, quorum_hash)["height"], expected_base)
        return quorum_hash

    def walk_first_epoch(self, node):
        """From 1175 to 1199, every phase of both protocols waited for.

        The Sentinel epoch and the Q60 DKG cycle share a grid, on this chain
        and on the release networks, so the first epoch runs beside a DKG.
        mine_quorum would pace that DKG and mine straight through the epoch's
        cutoff while doing it; this walks the same DKG phases with the epoch's
        own waits between them. Returns the quorum the DKG formed, the block
        position the threshold signature was there at, and node 0's report
        count.
        """
        # As mine_quorum reads them: with SPORK_21 off a member is asked for two
        # quorum connections, and with SPORK_23 off no probes are waited for.
        sporks = node.spork("show")
        connections = (Q60_SIZE - 1) if sporks["SPORK_21_QUORUM_ALL_CONNECTED"] <= 1 else 2
        assert sporks["SPORK_23_QUORUM_POSE"] > 1
        mns = self.mninfo

        assert_equal(node.getblockcount(), DSL_START - 1)
        self.bump_mocktime(1)
        base = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        assert_equal(node.getblockcount(), DSL_START)

        self.log.info("AT %d: the Sentinel layer is active on every node, in epoch %d, based on this block",
                      DSL_START, FIRST_EPOCH)

        def in_first_epoch(n):
            dsl = n.dslstatus()
            return dsl if dsl["epoch"] == FIRST_EPOCH else None
        for dsl in self.wait_on_every_node("the first epoch", in_first_epoch, 60).values():
            assert_equal((dsl["active"], dsl["enforcing"]), (True, False))
            assert_equal(dsl["epochblockhash"], base)

        self.log.info("  the DKG of the same window: phase 1 (init)")
        self.wait_for_quorum_phase(base, 1, Q60_SIZE, None, 0, mns, llmq_type_name=Q60_NAME)
        self.wait_for_quorum_connections(base, connections, mns, wait_proc=lambda: self.bump_mocktime(1),
                                         llmq_type_name=Q60_NAME)

        self.log.info("  announcements: all %d masternodes, seen by all %d nodes", MN_COUNT, len(self.nodes))
        self.bump_mocktime(60)
        self.wait_on_every_node("the announcements", lambda n: n.dslstatus()["respondedcount"] == MN_COUNT,
                                ANNOUNCE_TIMEOUT)
        self.mark("every announcement seen everywhere")

        for phase, field, count in ((2, "receivedContributions", Q60_SIZE),
                                    (3, "receivedComplaints", 0),
                                    (4, "receivedJustifications", 0),
                                    (5, "receivedPrematureCommitments", Q60_SIZE),
                                    (6, None, 0)):
            self.move_blocks(self.nodes, 2)
            self.log.info("  the DKG of the same window: phase %d", phase)
            self.wait_for_quorum_phase(base, phase, Q60_SIZE, field, count, mns, llmq_type_name=Q60_NAME)
        self.wait_for_quorum_commitment(base, self.nodes, llmq_type=Q60_TYPE)
        self.bump_mocktime(1)
        node.getblocktemplate()  # this calls CreateNewBlock
        self.generate(node, 1, sync_fun=self.sync_blocks)
        # One more block if the commitment missed the first, never as far as the cutoff.
        for _ in range(3):
            if base in node.quorum("list").get(Q60_NAME, []):
                break
            self.bump_mocktime(1)
            self.generate(node, 1, sync_fun=self.sync_blocks)
        assert base in node.quorum("list")[Q60_NAME], "the DKG beside the first epoch did not form a quorum"
        assert node.getblockcount() < DSL_START + CUTOFF
        self.mark("the DKG beside the first epoch formed its quorum")

        self.log.info("  the cutoff (+%d): reports, and the verdict they add up to on every node", CUTOFF)
        self.bump_mocktime(30)
        self.generate(node, DSL_START + CUTOFF - node.getblockcount(), sync_fun=self.sync_blocks)

        reports = MN_COUNT * SENTINELS

        def pool_complete(n):
            dsl = n.dslstatus()
            return dsl if dsl["epochreports"] == reports else None
        pools = self.wait_on_every_node("the report pool", pool_complete, VERDICT_TIMEOUT)
        for dsl in pools.values():
            assert_equal((dsl["onlinereports"], dsl["missedreports"]), (reports, 0))
            candidate = dsl["candidate"]
            assert_equal((candidate["version"], candidate["missedcount"], candidate["unobservedcount"]), (2, 0, 0))
        # equal on two nodes iff their pools hold the same reports
        assert_equal(len({dsl["poolhash"] for dsl in pools.values()}), 1)
        asked = time.time()
        node.dslstatus()
        self.dslstatus_cost = time.time() - asked
        self.mark("%d reports, all online, one pool and one verdict on every node (one dslstatus: %.2f s)"
                  % (reports, self.dslstatus_cost))

        self.log.info("  the signing window (+%d..+%d): the threshold signature under %s", SIGNING, EPOCH - 1, Q60_NAME)
        self.bump_mocktime(10)
        self.generate(node, SIGNING - CUTOFF, sync_fun=self.sync_blocks)
        signed_at = None
        for pos in range(SIGNING, EPOCH):
            assert_equal(node.getblockcount(), DSL_START + pos)
            if self.wait_until(lambda: self.dsl_signature_recovered(node, FIRST_EPOCH),
                               timeout=SIGN_TIMEOUT_PER_TICK, sleep=1, do_assert=False):
                signed_at = pos
                break
            if pos < EPOCH - 1:
                # a member that has signed asks again on every later block of the window
                self.generate(node, 1, sync_fun=self.sync_blocks)
        assert signed_at is not None, "no threshold signature for the first epoch by its last block"
        self.mark("the threshold signature was there at +%d" % signed_at)

        if node.getblockcount() < FIRST_COMMITMENT - 1:
            self.generate(node, FIRST_COMMITMENT - 1 - node.getblockcount(), sync_fun=self.sync_blocks)
        assert_equal(node.getblockcount(), FIRST_COMMITMENT - 1)
        return base, signed_at, reports

    # ---- the run -------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.started = time.time()
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        # SPORK_17 defaults to OFF, and without it CDKGSessionManager::UpdatedBlockTip
        # returns before any session handler: the phase thread sits in
        # WaitForNextPhase forever and it looks exactly like a quorum that will
        # not form. SPORK_2/3/19 are already on from the framework's setup.
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        assert_equal(len(node.masternodelist("status")), MN_COUNT)
        assert_equal((H % Q60_DKG_INTERVAL, DSL_START % EPOCH), (0, 0))
        height = node.getblockcount()
        assert height < FIRST_ENABLED_TIP - Q60_DKG_INTERVAL, (
            "setup left the chain at %d, too close to the formation lead at %d -- raise H" % (height, LEAD_START))

        self.log.info("Every node holds the one number: the Sentinel height derived from it, and no enforcement height")
        self.assert_sentinel_dormant()

        # ---- below the formation lead ------------------------------------

        self.log.info("Form a legacy (llmq_test) quorum below the formation lead")
        self.mine_quorum(llmq_type_name=LEGACY_NAME, llmq_type=LEGACY_TYPE,
                         expected_members=3, expected_connections=2,
                         expected_contributions=3, expected_commitments=3)
        assert node.getblockcount() < FIRST_ENABLED_TIP, "the legacy quorum ran into the formation lead; raise H"

        self.log.info("BELOW the lead (%d): a ChainLock forms, on llmq_test, and no llmq_defcon session runs", LEAD_START)
        below = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        self.wait_for_chainlock(node, below)
        old_lock = node.getbestchainlock()
        assert old_lock["height"] < LEAD_START
        assert_equal(old_lock["llmqType"], LEGACY_NAME)
        assert node.verifychainlock(old_lock["blockhash"], old_lock["signature"], old_lock["height"])
        self.assert_lock_profile(node, old_lock, expect_q60=False)
        for mn in self.mninfo:
            assert all(s["llmqType"] != Q60_NAME for s in mn.node.quorum("dkgstatus")["session"])
        self.mark("legacy ChainLock at %d saved" % old_lock["height"])

        # ---- the formation lead ------------------------------------------

        self.log.info("The profile is enabled for the block after %d, and not a block earlier", FIRST_ENABLED_TIP)
        self.mine_to(node, FIRST_ENABLED_TIP - 1)
        assert Q60_NAME not in node.quorum("list")
        self.mine_to(node, FIRST_ENABLED_TIP)
        assert Q60_NAME in node.quorum("list")

        self.log.info("INSIDE the lead [%d, %d): every one of its %d cycles forms a real %d-member quorum",
                      LEAD_START, H, LEAD_ROUNDS, Q60_SIZE)
        lead_quorums = []
        for i in range(LEAD_ROUNDS):
            lead_quorums.append(self.form_q60(node, LEAD_START + i * Q60_DKG_INTERVAL))
            self.mark("Q60 quorum %d of %d formed" % (i + 1, LEAD_ROUNDS))
        assert node.getblockcount() <= H - 1, (
            "the last round of the lead ran to %d, past H - 1; nothing is left to observe the pause on" % node.getblockcount())
        for entry in node.quorum("listextended")[Q60_NAME]:
            quorum_hash, info = list(entry.items())[0]
            assert quorum_hash in lead_quorums
            assert_greater_than_or_equal(info["numValidMembers"], Q60_MIN_SIZE)
            self.log.info("  %s numValidMembers=%d", quorum_hash[:16], info["numValidMembers"])

        # ---- H - 1: the last block of the pause --------------------------

        self.mine_to(node, H - 1)
        self.log.info("AT H - 1 (%d): Q60 must NOT sign an InstantSend request, and the tip must NOT be ChainLocked", H - 1)
        early = node.sendtoaddress(node.getnewaddress(), 1)
        self.sync_mempools()
        assert not self.q60_signed(node, early, timeout=NO_Q60_ISLOCK_WAIT), \
            "Q60 signed an InstantSend request below the activation height"
        # The control for this absence is the run itself: the same llmq_test
        # profile locked a block below the lead, its quorums formed in the same
        # windows as the Q60 ones, and the pause is the only thing holding it.
        assert not self.wait_for_chainlock(node, node.getbestblockhash(), expected=False, timeout=NO_CL_AT_PAUSE_WAIT), \
            "the last block of the formation lead was ChainLocked; the pause is not in effect"
        best = node.getbestchainlock()
        assert best["height"] < LEAD_START, "the best ChainLock entered the formation lead"
        assert_equal(best["llmqType"], LEGACY_NAME)
        assert_equal(node.getblockcount(), H - 1)

        # ---- H ----------------------------------------------------------

        self.log.info("AT H (%d), on a tip that is not mined past: the ChainLock, on %s", H, Q60_NAME)
        self.bump_mocktime(1)
        asked = time.time()
        h_hash = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        assert_equal(node.getblockcount(), H)
        self.wait_for_chainlock(node, h_hash, timeout=CL_AT_H_TIMEOUT)
        cl_wait = time.time() - asked
        new_lock = node.getbestchainlock()
        assert_equal((new_lock["height"], new_lock["blockhash"], new_lock["llmqType"]), (H, h_hash, Q60_NAME))
        assert node.verifychainlock(new_lock["blockhash"], new_lock["signature"], new_lock["height"])
        self.assert_lock_profile(node, new_lock, expect_q60=True)
        self.wait_until(lambda: self.all_nodes(lambda n: (self.best_chainlock(n) or {}).get("blockhash") == h_hash),
                        timeout=60, sleep=1)
        self.log.info("  the lock from below the lead still verifies")
        assert node.verifychainlock(old_lock["blockhash"], old_lock["signature"], old_lock["height"])
        assert_equal(node.getblockcount(), H)
        self.mark("block H ChainLocked on %s after %.1f s, on every node" % (Q60_NAME, cl_wait))

        self.log.info("AT H, the same tip: InstantSend signs on %s", Q60_NAME)
        asked = time.time()
        late = node.sendtoaddress(node.getnewaddress(), 1)
        self.sync_mempools()
        self.wait_for_internal_islock(node, late)
        is_wait = time.time() - asked
        assert self.q60_signed(node, late, timeout=30), \
            "the transaction locked, but not on Q60 -- the InstantSend resolver did not flip"
        islocks = node.getislocks([late])
        assert_equal((len(islocks), islocks[0]["txid"]), (1, late))
        assert_equal(node.getblock(islocks[0]["cycleHash"])["height"] % Q60_DKG_INTERVAL, 0)
        assert_equal(node.getblockcount(), H)
        self.mark("InstantSend locked on %s after %.1f s, still at H" % (Q60_NAME, is_wait))

        self.log.info("AT H the Sentinel layer is still dormant on every node")
        self.assert_sentinel_dormant()

        # ---- H .. the run-up ---------------------------------------------

        self.log.info("Mine to %d with the DKG spork off (see the file comment)", RUNUP_FIRST_BASE - 1)
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        # In whole cycles: every block here is still ChainLocked by 60 members,
        # which is what this span costs, and the handler coalesces a burst.
        self.mine_to(node, RUNUP_FIRST_BASE - 1, chunk=Q60_DKG_INTERVAL)
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        assert_equal(node.getblockcount(), RUNUP_FIRST_BASE - 1)
        self.mark("unpaced span mined")

        self.log.info("The %d cycles before the Sentinel start: a whole fresh active set", RUNUP_ROUNDS)
        runup_quorums = []
        for i in range(RUNUP_ROUNDS):
            runup_quorums.append(self.form_q60(node, RUNUP_FIRST_BASE + i * Q60_DKG_INTERVAL))
            self.mark("run-up quorum %d of %d formed" % (i + 1, RUNUP_ROUNDS))
        assert node.getblockcount() <= DSL_START - 1, (
            "the last run-up round ran to %d, past the Sentinel start; the first epoch was not walked" % node.getblockcount())
        assert_equal(sorted(node.quorum("list")[Q60_NAME]), sorted(runup_quorums))

        self.mine_to(node, DSL_START - 1)
        self.log.info("AT %d, one block below its start, the Sentinel layer is dormant on every node", DSL_START - 1)
        self.assert_sentinel_dormant()
        tip = node.getbestblockhash()
        self.wait_for_chainlock(node, tip)
        assert_equal(node.getbestchainlock()["llmqType"], Q60_NAME)

        # ---- the first epoch ---------------------------------------------

        epoch_quorum, signed_at, reports = self.walk_first_epoch(node)

        self.log.info("AT %d: the first service commitment", FIRST_COMMITMENT)
        self.bump_mocktime(10)
        boundary = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        assert_equal(node.getblockcount(), FIRST_COMMITMENT)
        c, msg_hash, signature = self.commitment_at(node, FIRST_COMMITMENT)
        self.log.info("  %s", c)
        base_hash = node.getblockhash(DSL_START)
        order = canonical([m["proRegTxHash"] for m in node.protx("diff", 1, DSL_START)["mnList"]])
        assert_equal(len(order), MN_COUNT)
        assert_equal(c["version"], 2)
        assert_equal(c["epoch"], FIRST_EPOCH)
        assert_equal(c["epochBlockHash"], base_hash)
        assert_equal(c["llmqType"], Q60_TYPE)  # the number, not getbestchainlock's name for it
        assert_equal(c["size"], len(order))
        assert_equal((c["missedCount"], c["missedIndices"]), (0, []))
        assert_equal(c["unobservedIndices"], [])
        assert_equal(c["observedCount"], c["size"])
        # The epoch selects its quorum eight blocks below its base: from the
        # run-up's four, not the one that formed beside the epoch.
        assert c["quorumHash"] in runup_quorums
        assert c["quorumHash"] != epoch_quorum
        request_id = self.dsl_request_id(node, FIRST_EPOCH)
        assert node.quorum("hasrecsig", Q60_TYPE, request_id, msg_hash)
        assert_equal(self.signers_of(node, Q60_TYPE, Q60_NAME, request_id, msg_hash, signature), [c["quorumHash"]])
        assert_equal(self.signers_of(node, LEGACY_TYPE, LEGACY_NAME, request_id, msg_hash, signature), [])
        # and the question can be answered no: the same signature over another message
        other = "%064x" % (int(msg_hash, 16) ^ 1)
        assert not node.quorum("verify", Q60_TYPE, request_id, other, signature, c["quorumHash"])

        self.log.info("  every node accepted the block, nobody carries a penalty, and the ChainLock covers it")
        for n in self.nodes:
            assert_equal(n.getbestblockhash(), boundary)
        self.wait_until(lambda: node.dslstatus()["epoch"] == FIRST_EPOCH + 1, timeout=30)
        listed = node.protx("list", "registered", True)
        assert_equal(len(listed), MN_COUNT)
        for mn in listed:
            state = mn["state"]
            assert_equal((state["PoSePenalty"], state["PoSeBanHeight"], state["missedServiceEpochs"]), (0, -1, 0))
        self.wait_for_chainlock(node, boundary)
        assert_equal(node.getbestchainlock()["llmqType"], Q60_NAME)
        self.mark("first service commitment mined and checked")

        # ---- re-validation -----------------------------------------------

        self.log.info("The controller re-validates the whole chain from its own block files")
        quorums_before = node.quorum("list", 100)
        # Block times run a little ahead of the mock clock; a restarted node
        # must find every one of them in its past. The framework starts a node
        # on the mock time it holds for it, which set_node_times keeps current.
        self.bump_mocktime(60)
        set_node_times(self.nodes, self.mocktime)
        asked = time.time()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: node.getblockcount() == FIRST_COMMITMENT and node.getbestblockhash() == boundary,
                        timeout=REINDEX_TIMEOUT, sleep=1)
        reindex_wait = time.time() - asked
        self.log.info("  same tip after %.1f s", reindex_wait)
        assert_equal(node.quorum("list", 100), quorums_before)
        assert node.verifychainlock(old_lock["blockhash"], old_lock["signature"], old_lock["height"])
        assert node.verifychainlock(new_lock["blockhash"], new_lock["signature"], new_lock["height"])
        self.assert_lock_profile(node, old_lock, expect_q60=False)
        self.assert_lock_profile(node, new_lock, expect_q60=True)
        again, msg_hash_again, signature_again = self.commitment_at(node, FIRST_COMMITMENT)
        assert_equal((again, msg_hash_again, signature_again), (c, msg_hash, signature))

        self.log.info("  and it drives the network again: the next block is accepted everywhere and ChainLocked")
        force_finish_mnsync(node)
        # FROM the controller, not to it as in the setup. Back up, it opens
        # quorum connections to masternodes by itself, and a masternode that
        # then dials it as well is refused: its MNAUTH "has already verified",
        # the new connection is dropped, and connect_nodes waits for an inbound
        # peer that will never be there. Asked from this side, a connection
        # that already exists is simply found.
        for mn in self.mninfo:
            self.connect_nodes(0, mn.nodeIdx)
        self.bump_mocktime(1)
        after = self.generate(node, 1, sync_fun=self.sync_blocks)[0]
        self.wait_for_chainlock(node, after)
        assert_equal(node.getbestchainlock()["llmqType"], Q60_NAME)

        self.log.info("RECORD H=%d lead=[%d,%d) lead_quorums=%d cl_at_h=%.1fs is_at_h=%.1fs dsl_start=%d "
                      "reports=%d dslstatus=%.2fs signed_at=+%d first_commitment=%d quorum=%s reindex=%.1fs wall=%ds",
                      H, LEAD_START, H, len(lead_quorums), cl_wait, is_wait, DSL_START, reports, self.dslstatus_cost,
                      signed_at, FIRST_COMMITMENT, c["quorumHash"][:16], reindex_wait, time.time() - self.started)


if __name__ == "__main__":
    V23JointActivationLLMQTest().main()
