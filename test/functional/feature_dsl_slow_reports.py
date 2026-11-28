#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""In the Sentinel wave a slow node's pong goes ahead of the reports it is still checking.

Seven masternodes and a producer on regtest (feature_dsl_service.py). One masternode, D, is restarted with
-dsltestprocessdelay: every report it reads costs it that much more -- on the pre-verifier's thread when that thread
started the report's job, else where the report is processed (SF1-C2) -- and so does every own report it signs. After
the restart D's one connection to the network is to another masternode, R, and D has authenticated it (mnauth):
whatever D takes from the network, a verified masternode peer relayed. A test connection listens to D alone: whatever
reaches it, D relayed. The first epoch after the restart is a warm-up,
in which D files no reports and its wave window stays shut; the test mines through it and on to the next epoch's
emission block (position 18). There D signs and relays its own reports, marks its observation of the epoch done, and
its window opens; its peers' reports wait in its receive queue while its one pre-verification thread works through
them.

The evidence, from D's log unless said otherwise:
1. The window open: the gate holds a head report of R, D's verified masternode peer (getpeerinfo), whose job has not
   finished (required before the ping).
2. Then, with no new block, D is asked to ping (the ping RPC). For R, matched by the ping's nonce: the ping sent at
   the RPC's request, the pong taken ahead of a head report of R whose job had not finished (pending or running; the
   held one or a later one of R's), and the pong's nonce matched -- in that order, the match within 2 s of the start
   of the RPC call (the test's clock against the log's own timestamp, both the system clock; the gate judges a
   report's age by the monotonic clock, and -mocktime touches neither), and before D had admitted all its peers'
   reports.
3. The pre-verification at work on one report: the thread's cache miss, its insert and the admission's hit, with
   the same entry id, in that order.
4. The whole wave through D: every report of its peers reached it through R, and the connection that listens to D
   alone is sent every report of the epoch, byte for byte -- the ones D took from R and its own: the pool digest of
   what it holds is D's -- and on the way D neither dropped a waiting job nor refused one for a full queue.
Finally every node holds every report of the epoch, and the pools agree. Without the gate (the window never opening)
step 1 fails, and the pong waits behind every report queued before it, each read at the test delay.
"""

from datetime import datetime, timezone
import re
import struct
import time

from feature_dsl_service import CUTOFF, EPOCH_INTERVAL, DSLServiceTest
from test_framework.messages import hash256
from test_framework.p2p import MESSAGEMAP, P2PInterface, p2p_lock
from test_framework.util import assert_equal, force_finish_mnsync

# What D's one checker and its message thread each spend on a report. Long enough for the wave to queue up behind
# them (the foreign reports take the checker several seconds), short enough that the two rounds of D's message thread a
# ping and its pong need stay well inside the two seconds below, measured from the RPC.
DELAY_MS = 100
SLOW_ARGS = [f"-dsltestprocessdelay={DELAY_MS}", "-dslperf=1"]
PONG_LIMIT_S = 2.0
BLS_SIG_SIZE = 96
REPORT_SIZE = 4 + 32 + 32 + 1 + BLS_SIG_SIZE  # CPoSeServiceReport::WIRE_SIZE
STAMP = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?)Z ")


def stamp(line):
    """The log's own timestamp of a line (UTC), in seconds since the epoch."""
    m = STAMP.match(line)
    assert m, f"no timestamp on: {line}"
    return datetime.fromisoformat(m.group(1)).replace(tzinfo=timezone.utc).timestamp()


class msg_posereport:
    """CPoSeServiceReport on the wire: nEpoch, target, sentinel, status, a basic-scheme BLS signature; and its bytes."""
    __slots__ = ("nEpoch", "target", "sentinel", "status", "sig", "raw")
    msgtype = b"posereport"

    def __init__(self):
        self.nEpoch = 0
        self.target = 0
        self.sentinel = 0
        self.status = 0
        self.sig = b"\x00" * BLS_SIG_SIZE
        self.raw = b""

    def deserialize(self, f):
        # The whole payload: a report followed by anything else is kept as it came, and fails the length check below.
        self.raw = f.read()
        self.nEpoch = struct.unpack("<I", self.raw[0:4])[0]
        self.target = int.from_bytes(self.raw[4:36], "little")
        self.sentinel = int.from_bytes(self.raw[36:68], "little")
        self.status = self.raw[68]
        self.sig = self.raw[69:REPORT_SIZE]

    def __repr__(self):
        return "msg_posereport(nEpoch=%d, target=%064x, sentinel=%064x)" % (self.nEpoch, self.target, self.sentinel)


MESSAGEMAP[b"posereport"] = msg_posereport
# The other Sentinel messages a node floods to its peers: this test's connection ignores them.
for _msgtype in (b"poseresp", b"posecommit", b"posechal"):
    MESSAGEMAP.setdefault(_msgtype, None)


class ReportListener(P2PInterface):
    """Keeps the reports the node it is connected to relays."""
    def __init__(self):
        super().__init__()
        self.reports = []

    def on_posereport(self, message):
        self.reports.append(message)

    def of_epoch(self, epoch):
        """The (target, sentinel) pairs of the epoch's reports received so far."""
        with p2p_lock:
            return {(r.target, r.sentinel) for r in self.reports if r.nEpoch == epoch}

    def poolhash(self, epoch):
        """The pool digest of the epoch's reports received so far, whole -- status and signature too -- computed as
        dslstatus computes it from the reports a node holds: the hash of each report's bytes, the hashes in byte order,
        hashed together (src/rpc/evo.cpp)."""
        with p2p_lock:
            raws = {r.raw for r in self.reports if r.nEpoch == epoch}
        return hash256(b"".join(sorted(hash256(raw) for raw in raws)))[::-1].hex()

    def sizes(self, epoch):
        """The payload sizes of the epoch's reports received so far."""
        with p2p_lock:
            return {len(r.raw) for r in self.reports if r.nEpoch == epoch}

    def received(self, epoch):
        """How many of the epoch's reports were received so far, each copy counted."""
        with p2p_lock:
            return sum(1 for r in self.reports if r.nEpoch == epoch)


class DSLSlowReportsTest(DSLServiceTest):
    def set_test_params(self):
        super().set_test_params()

    def log_lines(self, node, start):
        """D's debug log from byte `start` on."""
        with open(node.debug_log_path, "rb") as f:
            f.seek(start)
            return f.read().decode("utf-8", errors="replace").splitlines()

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

    def full_pool(self, epoch):
        status = [n.dslstatus() for n in self.nodes]
        return (all(s["epoch"] == epoch and s["epochreports"] == len(self.mninfo) * (len(self.mninfo) - 1) for s in status)
                and len({s["poolhash"] for s in status}) == 1)

    def run_test(self):
        producer = self.nodes[0]
        self.wait_for_sporks_same()
        for node in self.nodes:
            force_finish_mnsync(node)

        mn = self.mninfo[1]
        relay = self.mninfo[2]
        self.log.info("D is node %d; restarting it with -dsltestprocessdelay=%d", mn.nodeIdx, DELAY_MS)
        self.stop_node(mn.nodeIdx)
        self.start_masternode(mn, SLOW_ARGS)
        # The restarted node came back with the mocktime it was stopped at (feature_dsl_service.py): clock first,
        # connection second. Its one connection to the network is to another masternode, R: whatever D takes from
        # the network from here on, R relayed. (No quorum is formed in this test, so the masternodes open no
        # connections of their own.)
        self.bump_mocktime(1)
        self.connect_nodes(mn.nodeIdx, relay.nodeIdx)
        self.sync_blocks()
        slow = self.nodes[mn.nodeIdx]
        # D has authenticated R (mnauth): the peer its reports come from is a verified masternode.
        relay_peer = None

        def authenticated():
            nonlocal relay_peer
            relay_peer = next((p for p in slow.getpeerinfo() if p.get("verified_proregtx_hash") == relay.proTxHash), None)
            return relay_peer is not None

        self.wait_until(authenticated, timeout=30)
        relay_id = str(relay_peer["id"])
        self.log.info("D's connection to the network is to masternode R (node %d), verified, peer=%s", relay.nodeIdx, relay_id)
        listener = slow.add_p2p_connection(ReportListener())

        self.log.info("Through the warm-up epoch the restart began in, to a whole epoch D observes")
        self.next_boundary(producer)
        epoch = self.next_boundary(producer)
        self.bump_mocktime(1)
        self.generate(producer, CUTOFF - 1)
        assert_equal(producer.getblockcount() % EPOCH_INTERVAL, CUTOFF - 1)

        self.log.info("The emission block: D files its reports, and its peers' reports queue up behind its one checker")
        start = slow.debug_log_path.stat().st_size
        self.bump_mocktime(1)
        self.generate(producer, 1, sync_fun=self.no_op)
        held = re.compile(r"DSL gate -- holding report seq=(\d+) job=(pending|running), peer=(\d+)")
        found_hold = None

        def hold():
            nonlocal found_hold
            for line in self.log_lines(slow, start):
                m = held.search(line)
                if m and m.group(3) == relay_id:
                    found_hold = (int(m.group(1)), m.group(3))
                    return True
            return False

        # 1. Required: the window open, with an unfinished job at the head of the masternode peer's queue.
        self.wait_until(hold, timeout=30, sleep=0.05)
        held_seq, held_peer = found_hold
        self.log.info("The gate holds report seq=%d of masternode peer=%s", held_seq, held_peer)

        self.log.info("A ping, asked by RPC: its pong overtakes the reports D has not finished checking")
        ping_index = len(self.log_lines(slow, start))
        rpc_started = time.time()
        slow.ping()
        sent = re.compile(rf"sending ping nonce=([0-9a-f]{{16}}) \(rpc\), peer={held_peer}\b")
        found = None

        def evidence():
            nonlocal found
            lines = self.log_lines(slow, start)
            for i in range(ping_index, len(lines)):
                m = sent.search(lines[i])
                if not m:
                    continue
                nonce = m.group(1)
                overtook = re.compile(rf"DSL gate -- pong overtook report seq=(\d+) job=(pending|running) nonce={nonce}, peer={held_peer}\b")
                matched = re.compile(rf"pong nonce={nonce} matched, peer={held_peer}\b")
                j = next((k for k in range(i + 1, len(lines)) if overtook.search(lines[k])), None)
                if j is None:
                    continue
                k = next((k for k in range(j + 1, len(lines)) if matched.search(lines[k])), None)
                if k is None:
                    continue
                found = (nonce, int(overtook.search(lines[j]).group(1)), lines[k], lines[:k])
                return True
            return False

        # 2.
        self.wait_until(evidence, timeout=30, sleep=0.05)
        nonce, overtaken_seq, matched_line, upto = found
        elapsed = stamp(matched_line) - rpc_started
        assert overtaken_seq >= held_seq, f"the pong overtook seq={overtaken_seq}, before the held seq={held_seq}"
        # Two system-clock readings: a negative difference is a clock that went back, not a fast pong.
        assert 0 <= elapsed <= PONG_LIMIT_S, f"the pong of nonce {nonce} matched {elapsed:.3f} s after the ping RPC started"
        admitted = re.compile(rf"DSL -- posereport epoch={epoch} target=\S+ accepted=1, peer=")
        own = len(self.mninfo) - 1
        total = len(self.mninfo) * (len(self.mninfo) - 1)
        before = sum(1 for line in upto if admitted.search(line))
        assert before < total - own, f"D had admitted all {before} reports of its peers before the pong matched"
        # The peer all this happened with is still R's connection, a verified masternode.
        peer_now = next((p for p in slow.getpeerinfo() if str(p["id"]) == held_peer), None)
        assert peer_now is not None, "the held peer is gone"
        assert_equal(peer_now.get("verified_proregtx_hash"), relay.proTxHash)
        self.log.info("The pong of nonce %s from peer %s overtook report seq=%d (held: seq=%d) and matched %.3f s after "
                      "the ping RPC started, %d of %d peer reports admitted by then", nonce, held_peer, overtaken_seq,
                      held_seq, elapsed, before, total - own)

        self.log.info("Every report reaches every node, and the pools agree")
        self.sync_blocks()
        self.wait_until(lambda: self.full_pool(epoch), timeout=90)

        lines = self.log_lines(slow, start)
        # 3. One report warmed by the thread and admitted from the cache: miss, insert, hit, one entry id.
        events = {}
        for line in lines:
            for kind, pattern in (("miss", r"DSL sigcache -- preverify miss entry=([0-9a-f]{16})"),
                                  ("insert", r"DSL sigcache -- preverify insert entry=([0-9a-f]{16})"),
                                  ("hit", r"DSL sigcache -- net admit hit entry=([0-9a-f]{16})")):
                m = re.search(pattern, line)
                if m:
                    events.setdefault(m.group(1), []).append(kind)
        chained = [entry for entry, seq in events.items() if seq[:3] == ["miss", "insert", "hit"]]
        assert chained, "no report went from the thread's miss and insert to the admission's hit"
        self.log.info("%d report(s) warmed by the pre-verifier and admitted from the cache, e.g. entry %s",
                      len(chained), chained[0])

        # 4. Every report of D's peers reached D through R. The connection that listens to D alone was sent the whole
        # wave, byte for byte: D relayed every report it took from R, and its own, and what the listener holds is D's
        # pool -- the same digest as D's, status and signature included. And D neither dropped a waiting job to make
        # room nor refused one for a full queue.
        from_relay = re.compile(rf"DSL -- posereport epoch={epoch} target=\S+ accepted=1, peer={relay_id}\b")
        assert_equal(sum(1 for line in lines if from_relay.search(line)), total - own)
        assert_equal(sum(1 for line in lines if admitted.search(line)), total - own)
        self.wait_until(lambda: len(listener.of_epoch(epoch)) == total, timeout=60)
        sentinels = {sentinel for _, sentinel in listener.of_epoch(epoch)}
        assert_equal(len(sentinels), len(self.mninfo))
        d_protx = int(mn.proTxHash, 16)
        assert_equal(len([1 for _, sentinel in listener.of_epoch(epoch) if sentinel == d_protx]), own)
        status = slow.dslstatus()
        assert_equal(status["epoch"], epoch)
        assert_equal(listener.received(epoch), total)  # each report sent to it once
        assert_equal(listener.sizes(epoch), {REPORT_SIZE})
        assert_equal(listener.poolhash(epoch), status["poolhash"])
        log = self.log_lines(slow, start)
        assert not any("dropped to make room" in line for line in log), "D dropped a waiting job"
        queue_full = re.compile(r"DSL preverify -- no job for report seq=\d+: queue full, peer=")
        assert not any(queue_full.search(line) for line in log), "D refused a job for a full queue"
        self.log.info("The listener on D alone holds all %d reports of the epoch, %d of them D's own; pool digest %s, "
                      "as D's", total, own, status["poolhash"])


if __name__ == '__main__':
    DSLSlowReportsTest().main()
