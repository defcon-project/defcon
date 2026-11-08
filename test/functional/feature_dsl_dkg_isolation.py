#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""On a slow node, Sentinel work holds up neither the DKG nor its own message handling.

Seven masternodes and a producer on regtest; the ChainLock and DKG quorum is llmq_test, which has the timing of
llmq_defcon (24 blocks, two-block phases). The test mines one llmq_test quorum, the only one, so every epoch here
selects it to sign the epoch's commitment, and restarts one of its valid members, D, with -dsltestprocessdelay: every
Sentinel message D reads, every held announcement it checks and every report it signs costs it that much more, where
the work it stands for is done.

1. Copies. Byte-identical copies of an announcement D accepted, then a ping: the pong comes back within 2 s, D counts
   every copy as dropped unread, and the peer stays connected and unpunished. Read one by one, as before SF1-A, the
   copies alone would take COPIES x the delay.
2. The tick behind the DKG. D mines the cutoff block itself (position 18), so signing its reports falls in that
   block's tick. The DKG notification of that block starts within 0.25 s of the event being queued (the p99 of D's
   dkg_tip_start at position 18, from contrib/devtools/dsl_perf_pct.py), because the tick now runs after it; before,
   the tick ran first and its signing alone took (reports x the delay).
3. Commitments. For three epochs every boundary block carries a commitment for its epoch, the same on every node,
   signed by the quorum D is a valid member of, after D asked it to sign; D is observed online in it and nobody is
   missed -- D's reports are slow, not lost.
4. Shutdown. D mines its cutoff block once more and is stopped at once. Its log shows that block's event queued
   before the shutdown began and the block's tick, signing D's reports at the test delay, not finished by then; the
   node stops cleanly, with no error logged during the shutdown.
"""

import json
import os
import re
import struct
import subprocess
import sys
import time

from feature_dsl_service import CUTOFF, EPOCH_INTERVAL, DSLServiceTest
from test_framework.messages import ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.util import assert_equal, force_finish_mnsync

DELAY_MS = 200
SLOW_ARGS = [f"-dsltestprocessdelay={DELAY_MS}", "-dslperf=1", "-whitelist=noban@127.0.0.1"]
COPIES = 50
PONG_LIMIT_S = 2.0
DKG_START_LIMIT_NS = 250_000_000
BLS_SIG_SIZE = 96
LLMQ_TEST = 100


def canonical(hashes):
    """The order a commitment's bits index: uint256 memcmp, i.e. by internal (reversed) bytes."""
    return sorted(hashes, key=lambda h: bytes.fromhex(h)[::-1])


class msg_poseresp:
    """CPoSeServiceResponse on the wire: nEpoch, proTxHash, a basic-scheme BLS signature."""
    __slots__ = ("nEpoch", "proTxHash", "sig")
    msgtype = b"poseresp"

    def __init__(self, nEpoch=0, proTxHash=0, sig=None):
        self.nEpoch = nEpoch
        self.proTxHash = proTxHash
        self.sig = b"\x00" * BLS_SIG_SIZE if sig is None else sig

    def deserialize(self, f):
        self.nEpoch = struct.unpack("<I", f.read(4))[0]
        self.proTxHash = int.from_bytes(f.read(32), "little")
        self.sig = f.read(BLS_SIG_SIZE)

    def serialize(self):
        return struct.pack("<I", self.nEpoch) + ser_uint256(self.proTxHash) + self.sig

    def __repr__(self):
        return "msg_poseresp(nEpoch=%d, proTxHash=%064x)" % (self.nEpoch, self.proTxHash)


MESSAGEMAP[b"poseresp"] = msg_poseresp
# The other Sentinel messages a node floods to its peers: this test's connections ignore them.
for _msgtype in (b"posereport", b"posecommit", b"posechal"):
    MESSAGEMAP.setdefault(_msgtype, None)


class Capture(P2PInterface):
    """Keeps the announcements a node relays."""
    def __init__(self):
        super().__init__()
        self.responses = []

    def on_poseresp(self, message):
        self.responses.append(message)


class Quiet(P2PInterface):
    """Sends only; whatever the node relays back is ignored."""
    def on_poseresp(self, message):
        pass


class DSLDkgIsolationTest(DSLServiceTest):
    def set_test_params(self):
        super().set_test_params()
        # D: a valid member of the attesting quorum, chosen once the quorum exists (sentinelfix.md 6.2/3).
        self.slow_idx = None
        self.slow_protx = None
        self.quorum_hash = None

    def add_options(self, parser):
        parser.add_argument("--from-step", dest="from_step", type=int, default=1,
                            help="2 skips the copy flood (a negative control on the tree before SF1-A shows step 2 failing too)")

    def perf_lines(self, node, prefix):
        return [line for line in node.debug_log_path.read_text(encoding="utf-8").splitlines() if prefix in line]

    def slow_down_a_member(self, producer):
        """Mine the attesting quorum and restart one of its valid members, D, with the test delay."""
        self.quorum_hash = self.mine_quorum()
        # The only llmq_test quorum, so every epoch of this test selects it (SelectQuorumForSigningAt); the check of
        # each commitment below asserts it.
        assert_equal(producer.quorum("list")["llmq_test"], [self.quorum_hash])
        members = producer.quorum("info", LLMQ_TEST, self.quorum_hash)["members"]
        valid = [m["proTxHash"] for m in members if m["valid"]]
        mn = next(mn for mn in self.mninfo if mn.proTxHash in valid)
        self.slow_idx = mn.nodeIdx
        self.slow_protx = mn.proTxHash
        self.log.info("D is node %d, a valid member of quorum %s; restarting it with -dsltestprocessdelay=%d",
                      mn.nodeIdx, self.quorum_hash, DELAY_MS)
        self.stop_node(mn.nodeIdx)
        self.start_masternode(mn, SLOW_ARGS)
        # The restarted node came back with the mocktime it was stopped at (feature_dsl_service.py): clock first,
        # connection second.
        self.bump_mocktime(1)
        self.connect_nodes(mn.nodeIdx, 0)
        self.sync_blocks()
        return self.nodes[mn.nodeIdx]

    def next_boundary(self, producer):
        """Mine to the next epoch base and wait until every node took every masternode's announcement."""
        self.bump_mocktime(1)
        self.generate(producer, EPOCH_INTERVAL - producer.getblockcount() % EPOCH_INTERVAL)
        epoch = producer.getblockcount() // EPOCH_INTERVAL

        def everyone_heard():
            status = [n.dslstatus() for n in self.nodes]
            return all(s["epoch"] == epoch and s["respondedcount"] == len(self.mninfo) for s in status)

        self.wait_until(everyone_heard, timeout=90)
        return epoch

    def to_before_cutoff(self, producer):
        pos = producer.getblockcount() % EPOCH_INTERVAL
        if pos < CUTOFF - 1:
            self.bump_mocktime(1)
            self.generate(producer, CUTOFF - 1 - pos)
        assert_equal(producer.getblockcount() % EPOCH_INTERVAL, CUTOFF - 1)

    def check_commitment(self, producer, commitment, epoch):
        """Signed by the quorum D is a valid member of, after D asked it to sign, with D observed online: a zero
        missedCount alone would also hold for a D that nobody judged."""
        assert_equal(commitment["epoch"], epoch)
        assert_equal(commitment["quorumHash"], self.quorum_hash)
        members = producer.quorum("info", LLMQ_TEST, commitment["quorumHash"])["members"]
        assert any(m["proTxHash"] == self.slow_protx and m["valid"] for m in members), \
            f"D is no valid member of the quorum that signed epoch {epoch}"
        slow = self.nodes[self.slow_idx]
        asked = re.compile(rf"DSL -- asked quorum {self.quorum_hash} to sign epoch {epoch} \(format v\d+\)")
        assert any(asked.search(line) for line in self.perf_lines(slow, "DSL -- asked quorum")), \
            f"D did not ask the quorum to sign epoch {epoch}"
        order = canonical([m["proRegTxHash"] for m in producer.protx("diff", 1, epoch * EPOCH_INTERVAL)["mnList"]])
        assert_equal(len(order), commitment["size"])
        d = order.index(self.slow_protx)
        assert d not in commitment["unobservedIndices"], f"D (bit {d}) not observed in epoch {epoch}"
        assert d not in commitment["missedIndices"], f"D (bit {d}) missed in epoch {epoch}"
        assert_equal((commitment["missedCount"], commitment["missedIndices"], commitment["unobservedIndices"]), (0, [], []))
        self.log.info("Epoch %d: committed by quorum %s with D (bit %d) a valid member that asked it to sign, observed online",
                      epoch, self.quorum_hash[:16], d)

    def finish_epoch(self, producer, epoch, cutoff_miner=None):
        """From inside an epoch to its boundary block, which must carry its commitment, the same on every node."""
        self.to_before_cutoff(producer)
        self.bump_mocktime(1)
        self.generate(cutoff_miner or producer, 1)

        def complete_pool():
            status = [n.dslstatus() for n in self.nodes]
            return (all(s["epoch"] == epoch and s["epochreports"] == len(self.mninfo) * (len(self.mninfo) - 1) for s in status)
                    and len({s["poolhash"] for s in status}) == 1)

        self.wait_until(complete_pool, timeout=90)
        self.bump_mocktime(1)
        self.generate(producer, EPOCH_INTERVAL - 3 - producer.getblockcount() % EPOCH_INTERVAL)  # the first signing tick, +21
        self.wait_until(lambda: self.has_signature(producer, epoch), timeout=90)
        self.bump_mocktime(1)
        self.generate(producer, 3)
        boundary = producer.getbestblockhash()
        assert_equal(producer.getblockcount() % EPOCH_INTERVAL, 0)
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), boundary)
        block = producer.getblock(boundary, 2)
        commitments = [tx["poseServiceTx"]["commitment"] for tx in block["tx"] if tx.get("type") == 10]
        assert_equal(len(commitments), 1)
        self.check_commitment(producer, commitments[0], epoch)
        return producer.getblockcount() // EPOCH_INTERVAL

    def run_test(self):
        producer = self.nodes[0]
        producer.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        for node in self.nodes:
            force_finish_mnsync(node)
        slow = self.slow_down_a_member(producer)
        self.bump_mocktime(60)
        capture = producer.add_p2p_connection(Capture())

        epoch = self.next_boundary(producer)
        if self.options.from_step <= 1:
            epoch = self.step_copies(producer, slow, capture, epoch)
        else:
            self.log.info("Step 1 skipped (--from-step=%d)", self.options.from_step)
            epoch = self.finish_epoch(producer, epoch)
        epoch = self.step_tick_behind_dkg(producer, slow, epoch)
        self.step_shutdown(producer, slow, epoch)

    def step_copies(self, producer, slow, capture, epoch):
        self.log.info("Step 1: %d byte-identical copies of an accepted announcement, then a ping, to the slow node", COPIES)
        self.wait_until(lambda: any(r.nEpoch == epoch for r in capture.responses), timeout=30)
        copy = next(r for r in capture.responses if r.nEpoch == epoch)
        flooder = slow.add_p2p_connection(Quiet())
        flooder.sync_with_ping()
        start = time.monotonic()
        for _ in range(COPIES):
            flooder.send_message(copy)
        flooder.sync_with_ping(timeout=120)
        elapsed = time.monotonic() - start
        self.log.info("The pong came back after %.3f s (read one by one: at least %.1f s)", elapsed, COPIES * DELAY_MS / 1000)
        assert elapsed < PONG_LIMIT_S, f"{COPIES} copies took {elapsed:.3f} s"
        flooder.sync_with_ping()
        assert flooder.is_connected
        assert_equal(slow.listbanned(), [])

        self.log.info("Step 3, first epoch: its commitment, with the copies counted on the slow node")
        epoch = self.finish_epoch(producer, epoch)
        summary = re.compile(rf"DSL perf -- epoch={epoch - 1} base=\S+ .*duplicate_response_undecoded=(\d+)/")
        self.wait_until(lambda: any(summary.search(line) for line in self.perf_lines(slow, "DSL perf -- epoch=")), timeout=30)
        unread = max(int(summary.search(line).group(1)) for line in self.perf_lines(slow, "DSL perf -- epoch=") if summary.search(line))
        self.log.info("The slow node dropped %d announcement copies unread in epoch %d", unread, epoch - 1)
        assert unread >= COPIES, f"only {unread} copies were dropped unread"
        assert flooder.is_connected
        return epoch

    def step_tick_behind_dkg(self, producer, slow, epoch):
        self.log.info("Step 2: the slow node mines the cutoff block, so its report signing falls in that block's tick")
        self.wait_until(lambda: all(n.dslstatus()["respondedcount"] == len(self.mninfo) for n in self.nodes), timeout=90)
        emitted = re.compile(rf"DSL -- emitting (\d+) sentinel reports for epoch {epoch}\b")
        epoch_with_slow_cutoff = epoch
        epoch = self.finish_epoch(producer, epoch, cutoff_miner=slow)
        reports = [int(m.group(1)) for line in self.perf_lines(slow, "DSL -- emitting") for m in [emitted.search(line)] if m]
        assert reports and reports[-1] > 0, "the slow node signed no reports at its cutoff block"
        self.log.info("The slow node signed %d reports in its cutoff block's tick (%.1f s at the test delay)",
                      reports[-1], reports[-1] * DELAY_MS / 1000)
        hist = f"DSL perf hist -- epoch={epoch_with_slow_cutoff} "
        self.wait_until(lambda: any(" metric=dkg_tip_start pos=18 " in line for line in self.perf_lines(slow, hist)), timeout=30)
        script = os.path.join(self.config["environment"]["SRCDIR"], "contrib", "devtools", "dsl_perf_pct.py")
        result = subprocess.run([sys.executable, script, "--json", "--metric", "dkg_tip_start", "--epoch", str(epoch_with_slow_cutoff),
                                 str(slow.debug_log_path)], capture_output=True, text=True, check=True)
        row = next(r for r in json.loads(result.stdout) if r["pos"] == CUTOFF)
        self.log.info("dkg_tip_start at position %d on the slow node: n=%d, p99 <= %d ns", CUTOFF, row["n"], row["p99_ns"])
        assert not row["p99_lower_bound"]
        assert row["p99_ns"] <= DKG_START_LIMIT_NS, f"the DKG notification waited up to {row['p99_ns']} ns"

        self.log.info("Step 3, third epoch: its commitment too")
        self.wait_until(lambda: all(n.dslstatus()["respondedcount"] == len(self.mninfo) for n in self.nodes), timeout=90)
        return self.finish_epoch(producer, epoch)

    def step_shutdown(self, producer, slow, epoch):
        self.log.info("Step 4: the slow node mines its cutoff block once more and is stopped at once, that block's tick unfinished")
        self.wait_until(lambda: all(n.dslstatus()["respondedcount"] == len(self.mninfo) for n in self.nodes), timeout=90)
        self.to_before_cutoff(producer)
        self.sync_blocks()
        self.bump_mocktime(1)
        cutoff_hash = self.generate(slow, 1, sync_fun=self.no_op)[0]
        # Straight after the block: its tick, which signs the slow node's reports at DELAY_MS each, takes over a second.
        self.stop_node(self.slow_idx)
        # The log also holds the restart in slow_down_a_member, its own shutdown included: everything is looked up
        # from the cutoff block's event on.
        log = slow.debug_log_path.read_text(encoding="utf-8").splitlines()

        def first(pattern, start):
            return next((i for i in range(start, len(log)) if re.search(pattern, log[i])), None)

        queued = first(rf"Enqueuing UpdatedBlockTip: new block hash={cutoff_hash} ", 0)
        assert queued is not None, "the cutoff block's event is not in the log"
        shutdown = first(r"PrepareShutdown: In progress", queued)
        assert shutdown is not None, "no shutdown after the cutoff block's event was queued"
        done = first(r"Shutdown: done", shutdown)
        assert done is not None, "the shutdown did not finish"
        emitted = first(rf"DSL -- emitting \d+ sentinel reports for epoch {epoch}\b", queued)
        assert emitted is None or emitted > shutdown, "the cutoff block's tick had finished before the shutdown began"
        errors = [line for line in log[shutdown:done + 1] if "ERROR" in line or "EXCEPTION" in line]
        assert_equal(errors, [])
        if emitted is None:
            self.log.info("The shutdown began with the tick pending; the node stopped before running it, cleanly")
        else:
            self.log.info("The shutdown began with the tick running; it finished within the shutdown (%s), and the node stopped cleanly",
                          log[emitted].split("DSL -- ", 1)[1])


if __name__ == '__main__':
    DSLDkgIsolationTest().main()
