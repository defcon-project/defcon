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
                    no longer reproduces. The members relay the commitment
                    they signed, and the boundary carries that one -- the
                    quorum's verdict, not the miner's -- so the late reports
                    move no counter and the epoch is not lost.
  forged-commitment while the genuine signed commitment is in flight, a peer
                    sends variants of it -- a bit cleared under the genuine
                    signature, a quorum the epoch does not select, the version-1
                    format, a bitfield one bit short, an oversized payload; each
                    is refused for its own reason, a second peer of the same node
                    receives none of them, and the boundary still carries the
                    genuine one.
  quorum-member-skip one of three signing members withholding its share still
                    yields a commitment; two withholding yield none.
  miner-skip        the block producer leaves the commitment out; the epoch
                    closes without one, and the next epoch commits again.

Shadow mode throughout: counters move, penalties never do.

Timing rule the whole file obeys: a masternode announces its liveness at the
tick of the boundary block that opens an epoch, so a fault meant for an epoch
must be armed *before* that boundary is mined -- between `walk` and `close`.
"""

import struct
import time
from io import BytesIO

from test_framework.messages import deser_compact_size, ser_compact_size
from test_framework.p2p import MESSAGEMAP, P2PInterface, p2p_lock
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync

EPOCH = 24
CUTOFF = EPOCH - EPOCH // 4   # 18: reports are emitted from here
SIGNING = EPOCH - EPOCH // 8  # 21: the quorum is asked to sign from here
ARGS = ["-testactivationheight=dsl@1", "-enablefaultinjection=1"]
DSL_TX_TYPE = 10


class msg_posecommit:
    """A Sentinel commitment on the wire, kept as raw bytes: the test forges
    variants of a genuine one and never relies on the node's own parser."""
    __slots__ = ("payload",)
    msgtype = b"posecommit"

    def __init__(self, payload=b""):
        self.payload = payload

    def deserialize(self, f):
        self.payload = f.read()

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_posecommit(%d bytes)" % len(self.payload)


# DSL traffic an observer is flooded with but does not inspect
for _msgtype in (b"poseresp", b"posereport", b"posechal"):
    MESSAGEMAP.setdefault(_msgtype, None)
MESSAGEMAP[b"posecommit"] = msg_posecommit


class CommitmentObserver(P2PInterface):
    def __init__(self):
        super().__init__()
        self.commitments = []

    def on_posecommit(self, message):
        self.commitments.append(message.payload)


def read_bits(f):
    n = deser_compact_size(f)
    raw = f.read((n + 7) // 8)
    return [bool(raw[i // 8] >> (i % 8) & 1) for i in range(n)]


def write_bits(bits):
    raw = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        if bit:
            raw[i // 8] |= 1 << (i % 8)
    return ser_compact_size(len(bits)) + bytes(raw)


def parse_commitment(payload):
    """CPoSeServiceCommitment's SERIALIZE_METHODS: nVersion u16, nEpoch u32, epochBlockHash,
    llmqType u8, quorumHash, DYNBITSET missed, DYNBITSET observed (version 2 only), BLS signature."""
    f = BytesIO(payload)
    c = {}
    c["version"], c["epoch"] = struct.unpack("<HI", f.read(6))
    c["base"] = f.read(32)
    c["llmq"] = f.read(1)
    c["quorum"] = f.read(32)
    c["missed"] = read_bits(f)
    c["observed"] = read_bits(f) if c["version"] >= 2 else None
    c["sig"] = f.read(96)
    assert_equal(f.read(), b"")
    return c


def build_commitment(c):
    out = struct.pack("<HI", c["version"], c["epoch"]) + c["base"] + c["llmq"] + c["quorum"] + write_bits(c["missed"])
    if c["version"] >= 2:
        out += write_bits(c["observed"])
    return out + c["sig"]


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

    def assert_nothing_relayed(self, bystander, genuine_payload):
        """The node relays to every peer but the sender, so a forgery it forwarded would
        reach `bystander`. Everything the bystander holds must be the genuine commitment."""
        bystander.sync_with_ping()
        with p2p_lock:
            foreign = [p for p in bystander.commitments if p != genuine_payload]
        assert_equal(foreign, [])

    def forge_commitments(self, node, observer, bystander, genuine, genuine_payload):
        """Send `node` variants of the genuine commitment it already holds, each with a
        different hash (so no duplicate check hides it) and each broken in one way."""
        size = len(genuine["missed"])
        # clear one observed bit whose missed bit is clear: the shape stays legal, the signature does not
        at = next(i for i in range(size) if genuine["observed"][i] and not genuine["missed"][i])
        cleared = dict(genuine, observed=[bit and i != at for i, bit in enumerate(genuine["observed"])])
        forgeries = [
            ("a bit cleared under the genuine signature", cleared, "refused: bad-dsl-invalid-sig"),
            ("a quorum the epoch does not select", dict(genuine, quorum=bytes([0x5a]) * 32),
             "refused: not the quorum the epoch selects"),
            ("the version-1 format", dict(genuine, version=1), "refused: format v1 where the boundary requires v2"),
            ("a bitfield one bit short", dict(genuine, missed=genuine["missed"][:-1], observed=genuine["observed"][:-1]),
             "refused: a bitfield of %d for a list of %d" % (size - 1, size)),
        ]
        for what, forged, reason in forgeries:
            self.log.info("  forged commitment, %s: refused, not relayed", what)
            with node.assert_debug_log(expected_msgs=["signed commitment for epoch %d from peer=" % genuine["epoch"], reason],
                                       unexpected_msgs=["accepted from peer="]):
                observer.send_and_ping(msg_posecommit(build_commitment(forged)))
            self.assert_nothing_relayed(bystander, genuine_payload)
        self.log.info("  forged commitment, oversized: dropped before it is read")
        huge = dict(genuine, missed=[False] * 70000, observed=[False] * 70000)
        with node.assert_debug_log(expected_msgs=["oversized posecommit"],
                                   unexpected_msgs=["accepted from peer=", "signed commitment for epoch"]):
            observer.send_and_ping(msg_posecommit(build_commitment(huge)))
        self.assert_nothing_relayed(bystander, genuine_payload)

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
        self.log.info("diverged-pool: reports arriving after the signing offset; the boundary carries the commitment the quorum signed")
        late = [self.mn_node(m.proTxHash) for m in self.mninfo[1:3]]
        # connected an epoch ahead: a new connection earns its DSL budget block by block
        observer = node.add_p2p_connection(CommitmentObserver())
        bystander = node.add_p2p_connection(CommitmentObserver())  # never sends: sees what the node relays
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
        diverged_epoch = node.getblockcount() // EPOCH

        # ---- forged-commitment ---------------------------------------------
        # The members' relay has already carried the genuine commitment through
        # this node to the observer; forgeries of it must all be refused.
        self.log.info("forged-commitment: variants of the signed commitment in flight are refused and not relayed")

        def delivered():
            return [p for p in observer.commitments if parse_commitment(p)["epoch"] == diverged_epoch]
        observer.wait_until(lambda: len(delivered()) > 0, timeout=30)
        genuine_payload = delivered()[0]
        genuine = parse_commitment(genuine_payload)
        assert_equal(genuine["version"], 2)
        assert_equal(genuine["base"][::-1].hex(), node.getblockhash(diverged_epoch * EPOCH))
        # the bystander has the genuine commitment too before any forgery is sent
        bystander.wait_until(lambda: genuine_payload in bystander.commitments, timeout=30)
        self.forge_commitments(node, observer, bystander, genuine, genuine_payload)

        with node.assert_debug_log([
            "attached the DSL service commitment the quorum signed for epoch %d, relayed by its members "
            "(this producer's own pool diverged)" % diverged_epoch,
        ]):
            c = self.close(node)
        # ... but the quorum signed a pool without the late reports (4 < nDSLSentinelAgree), and that
        # signed commitment, relayed by the members, is what the boundary carries: nobody MISSED
        assert c is not None, "the boundary carried no commitment although the quorum had signed one"
        assert_equal(c["epoch"], diverged_epoch)
        assert_equal(c["missedCount"], 0)
        assert_equal(self.missed_epochs(node, target), 0)
        # what the quorum signed reached no verdict on the target (4 reports < nDSLSentinelAgree): unobserved,
        # neither MISSED nor ONLINE -- the late reports that would have made it MISSED move nothing
        order = canonical([m["proRegTxHash"] for m in node.protx("diff", 1, diverged_epoch * EPOCH)["mnList"]])
        assert order.index(target) in c["unobservedIndices"], (order.index(target), c["unobservedIndices"])
        # once its boundary block is connected, the genuine commitment itself is not even checked
        with node.assert_debug_log(["ignored: its boundary block is already connected"]):
            observer.send_and_ping(msg_posecommit(build_commitment(genuine)))
        node.disconnect_p2ps()
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
