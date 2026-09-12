#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A genuine early announcement survives a hold that peers have flooded.

The hold for early announcements (feature_dsl_early_announcement_hold.py) keeps
messages nobody can verify yet: the epoch's base block, and with it the
masternode list and the operator key, do not exist on this node until it
connects. Its capacity is therefore the one thing a peer can attack before
authentication -- fill it, and the genuine announcement arriving afterwards is
lost exactly as it was before the hold existed. Arrival order favours the
attacker structurally: junk can be sent as much as an epoch before the base,
while the genuine announcement only comes into being when the base is mined.

So the hold values an entry by what its LIVE vouchers -- the peers that
delivered a copy -- pushed into the contest the entry is in:

  - identical copies share one entry, and each peer bringing one joins its
    vouchers
  - when a choice is forced (the hold is full, or a masternode already has its
    cap of distinct payloads), the weakest gives way: an entry no live peer
    vouches for any more is weakest, then the one whose lightest voucher
    delivered the most of the entries being chosen among. Never the newcomer
    for being new; on a tie the incumbent stays, so a flooder cannot churn
  - a peer that disconnects stops vouching, so reconnecting buys nothing

Counting inside the contest is what makes it safe for an honest relayer: what
it forwards for other masternodes cannot make its entry look greedy here.

Three epochs, one phase each, against a real masternode's announcements
captured as it made them:

  1. one peer floods 4096 distinct junk announcements; the genuine one arrives
     from another peer and displaces one of the flooder's
  2. the masternode's own signatures from other epochs -- valid points over the
     wrong message -- replayed under this epoch by two attacking connections:
     the hold takes them all, because nothing in it can tell them from the real
     one, and the drain is where they are separated -- four refused for a bad
     signature, the genuine one accepted
  3. the attacker delivers the genuine announcement first, fills the rest, an
     honest peer repeats it, then the attacker disconnects and returns on a
     fresh connection with a full hold's worth of junk: its own orphaned
     entries are what it displaces, and the genuine one is accepted at the base
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
BLS_SIG_SIZE = 96
HOLD_MAX = 4096          # DSL_EARLY_RESPONSES_MAX
REPLAYS = 4              # replayed signatures used to crowd one masternode


class msg_poseresp:
    """CPoSeServiceResponse on the wire: nEpoch, proTxHash, a basic-scheme BLS
    signature. Parsed as well as sent, because the receiver relays a drained
    announcement back to every peer, this test's included."""
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


def junk_protx(i):
    # distinct per message, and none of them a masternode; the display form of
    # a uint256 is the big-endian hex of the integer, which %064x reproduces
    return (0xDEAD << 240) | i


class Capture(P2PInterface):
    """Collects the announcements a masternode relays to its peers."""
    def __init__(self):
        super().__init__()
        self.responses = []

    def on_poseresp(self, message):
        self.responses.append(message)


class Quiet(P2PInterface):
    """Sends only; the copy the receiver relays back after draining is ignored."""
    def on_poseresp(self, message):
        pass


class DSLEarlyAnnouncementHoldFloodTest(DashTestFramework):
    def set_test_params(self):
        args = ["-testactivationheight=dsl@1"]
        # The receiver's peers are whitelisted, which exempts them from the
        # per-peer budget at the DSL message entry
        # (feature_dsl_message_budget.py). What this test measures is the
        # hold's own rule at the hold's own bound, and the budget would stop a
        # single connection thousands of messages short of filling it. They are
        # layers, not alternatives: an attacker spread over enough connections,
        # or patient enough across blocks, still reaches the hold, and this is
        # what happens to the genuine announcement when it does.
        self.set_dash_test_params(3, 1, extra_args=[args, args + ["-whitelist=noban@127.0.0.1"], args])

    def catch_up(self, receiver, source, height, expected=None):
        with receiver.assert_debug_log(expected_msgs=expected or [], timeout=15):
            for h in range(receiver.getblockcount() + 1, height + 1):
                assert_equal(receiver.submitblock(source.getblock(source.getblockhash(h), 0)), None)
        assert_equal(receiver.getblockcount(), height)

    def flood(self, peer, epoch, first, count):
        for i in range(first, first + count):
            peer.send_message(msg_poseresp(epoch, junk_protx(i)))
        peer.sync_with_ping()

    def run_test(self):
        miner, receiver, masternode = self.nodes
        for node in self.nodes:
            force_finish_mnsync(node)
        # the receiver hears about blocks and announcements only from this
        # test's own peers, so its distance to each base is under control
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        assert_equal(receiver.getconnectioncount(), 0)

        capture = masternode.add_p2p_connection(Capture())
        attacker = receiver.add_p2p_connection(Quiet())
        relay = receiver.add_p2p_connection(Quiet())

        # the miner and the masternode run six epochs ahead while the receiver
        # stays behind; the masternode announces on each base's tick, and the
        # capture peer keeps every announcement it relays
        start = miner.getblockcount()
        first_base = (start // EPOCH_INTERVAL + 1) * EPOCH_INTERVAL
        if first_base - start < 8:
            first_base += EPOCH_INTERVAL
        bases = [first_base + EPOCH_INTERVAL * i for i in range(6)]
        genuine = {}
        for base in bases:
            epoch = base // EPOCH_INTERVAL
            self.generate(miner, base - miner.getblockcount(), sync_fun=lambda: self.sync_blocks([miner, masternode]))
            self.wait_until(lambda: any(r.nEpoch == epoch for r in capture.responses), timeout=30)
            genuine[epoch] = next(r for r in capture.responses if r.nEpoch == epoch)
        proTx = genuine[bases[0] // EPOCH_INTERVAL].proTxHash
        assert all(g.proTxHash == proTx for g in genuine.values())
        assert len({g.sig for g in genuine.values()}) == len(bases), "one distinct signature per epoch"
        self.log.info(f"masternode {proTx:064x} announced for epochs {sorted(genuine)}; receiver at {receiver.getblockcount()}")

        # ---- phase 1: one peer floods the hold; the genuine one still gets in
        base, epoch = bases[0], bases[0] // EPOCH_INTERVAL
        distance = base - receiver.getblockcount()
        assert 8 <= distance <= EPOCH_INTERVAL, distance
        self.log.info(f"phase 1, epoch {epoch}, {distance} blocks early: a flood of {HOLD_MAX} distinct junk announcements is held")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (junk_protx(HOLD_MAX - 1), distance)], timeout=30):
            self.flood(attacker, epoch, 0, HOLD_MAX)

        self.log.info("the flooder's next one is dropped: nothing here was pushed by fewer than it")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, dropped (hold full, no entry pushed here by fewer), peer="
                % (junk_protx(HOLD_MAX), distance)]):
            attacker.send_and_ping(msg_poseresp(epoch, junk_protx(HOLD_MAX)))

        self.log.info("a repeat brought by another peer costs nothing, and that peer joins its vouchers")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, already held, vouched for by 2 peer(s), peer=" % (junk_protx(0), distance)]):
            relay.send_and_ping(msg_poseresp(epoch, junk_protx(0)))

        self.log.info("the genuine announcement from another peer displaces one of the flooder's")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one whose lightest voucher pushed %d of this contest, peer="
                % (proTx, distance, HOLD_MAX)], timeout=15):
            relay.send_and_ping(genuine[epoch])

        self.log.info("the base connects: of the full hold, exactly the genuine announcement is accepted")
        self.catch_up(receiver, miner, base, [
            "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (HOLD_MAX, epoch),
            "accepted (1 responded so far)",
        ])
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        # ---- phase 2: many payloads under one masternode; the drain separates them
        base, epoch = bases[1], bases[1] // EPOCH_INTERVAL
        self.catch_up(receiver, miner, base - 10)
        distance = 10
        attacker2 = receiver.add_p2p_connection(Quiet())
        others = [genuine[e] for e in sorted(genuine) if e != epoch]
        self.log.info(f"phase 2, epoch {epoch}: {REPLAYS} replayed signatures crowd the masternode, each brought by two connections")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (proTx, distance)]):
            for other in others[:REPLAYS]:
                attacker.send_message(msg_poseresp(epoch, proTx, other.sig))
            attacker.sync_with_ping()
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, already held, vouched for by 2 peer(s), peer=" % (proTx, distance)]):
            for other in others[:REPLAYS]:
                attacker2.send_message(msg_poseresp(epoch, proTx, other.sig))
            attacker2.sync_with_ping()

        self.log.info("nothing here is a contest: the hold is not full, so a further payload is simply held too")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (proTx, distance)]):
            attacker.send_and_ping(msg_poseresp(epoch, proTx, others[REPLAYS].sig))

        self.log.info("and so is the genuine one, brought by a third peer -- no eviction, no choice made without evidence")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (proTx, distance)]):
            relay.send_and_ping(genuine[epoch])

        self.log.info(f"the base connects: the drain checks all {REPLAYS + 2} signatures, refuses {REPLAYS + 1}, accepts the genuine one")
        with receiver.assert_debug_log(expected_msgs=["refused: bad signature"] * (REPLAYS + 1) + [
                "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (REPLAYS + 2, epoch),
                "accepted (1 responded so far)"], timeout=15):
            self.catch_up(receiver, miner, base)
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        # ---- phase 3: the attacker owns the genuine entry first, then reconnects
        base, epoch = bases[2], bases[2] // EPOCH_INTERVAL
        self.catch_up(receiver, miner, base - 10)
        distance = 10
        self.log.info(f"phase 3, epoch {epoch}: the attacker delivers the genuine announcement first and fills the rest")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (proTx, distance)]):
            attacker.send_and_ping(genuine[epoch])
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (junk_protx(HOLD_MAX - 2), distance)], timeout=30):
            self.flood(attacker, epoch, 0, HOLD_MAX - 1)

        self.log.info("an honest peer repeats the genuine one: it now has a second voucher")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, already held, vouched for by 2 peer(s), peer=" % (proTx, distance)]):
            relay.send_and_ping(genuine[epoch])

        self.log.info("the attacker disconnects: its vouching goes with it, and its entries are orphaned")
        with receiver.assert_debug_log(expected_msgs=["Cleared nodestate for peer="], timeout=15):
            attacker.peer_disconnect()
            attacker.wait_for_disconnect()
            self.wait_until(lambda: receiver.getconnectioncount() == 2, timeout=30)
        attacker3 = receiver.add_p2p_connection(Quiet())

        self.log.info("back on a fresh connection, every junk it sends displaces one of its own orphans")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one whose lightest voucher pushed nothing live, peer="
                % (junk_protx(HOLD_MAX), distance)]):
            attacker3.send_and_ping(msg_poseresp(epoch, junk_protx(HOLD_MAX)))
        self.log.info(f"and the other {HOLD_MAX - 2} orphans go the same way; the genuine one, vouched for by a live honest peer, is never the weakest")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one whose lightest voucher pushed nothing live, peer="
                % (junk_protx(2 * HOLD_MAX - 2), distance)], timeout=30):
            self.flood(attacker3, epoch, HOLD_MAX + 1, HOLD_MAX - 2)

        self.log.info("with the orphans gone its next one is dropped, because only its own entries are left to displace")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, dropped (hold full, no entry pushed here by fewer), peer="
                % (junk_protx(2 * HOLD_MAX - 1), distance)]):
            attacker3.send_and_ping(msg_poseresp(epoch, junk_protx(2 * HOLD_MAX - 1)))

        self.log.info("the base connects: the genuine announcement is the one accepted")
        self.catch_up(receiver, miner, base, [
            "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (HOLD_MAX, epoch),
            "accepted (1 responded so far)",
        ])
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLEarlyAnnouncementHoldFloodTest().main()
