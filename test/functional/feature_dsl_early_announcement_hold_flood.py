#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A genuine early announcement survives a hold that a peer has flooded.

The hold for early announcements (feature_dsl_early_announcement_hold.py)
keeps messages nobody can verify yet: the epoch's base block, and with it the
masternode list and the operator key, do not exist on this node until it
connects. So the hold's capacity is the one thing a peer can attack before
authentication -- fill it with junk, and a genuine announcement arriving after
the fill is lost exactly as it was before the hold existed. The independent
review of the hold proved that with 4096 zero-signature copies from one peer,
and then, against a first fix that credited each entry to the peer that
delivered it first, with a reconnect: the attacker delivers the genuine
announcement first and owns its slot, an honest peer's copy changes nothing,
and after the attacker reconnects its old credit is what gets evicted -- the
genuine announcement with it.

What the hold does about it, checked here message by message, is to value an
entry by who vouches for it -- every LIVE peer that delivered a copy:

  - identical copies share one entry, and each peer bringing one joins its
    vouchers; the flood carries a genuine announcement through every honest
    peer, junk only through whoever made it up
  - when the hold is full, or a masternode already has its cap of distinct
    payloads, the weakest entry gives way: fewer live vouchers, then no
    seasoned voucher, then a lightest voucher carrying the most -- never the
    newcomer just because it is new, never a genuine one because a flooder
    got there first
  - a peer that disconnects stops vouching, so reconnecting starts from zero

Three epochs, one phase each, against a real masternode's announcements
captured as it made them:

  1. one peer floods 4096 distinct junk announcements; the genuine one arrives
     from another peer and is held in a flooder's place; the flooder's own
     next one is dropped; a repeat costs nothing
  2. the same masternode's announcement from other epochs -- valid signatures
     over the wrong message -- are replayed under this epoch: four are held,
     the fifth is dropped at the per-masternode cap, and the genuine one
     displaces one of them; the drain verifies four signatures, accepts one
  3. the review's reconnect: the attacker delivers the genuine announcement
     first, fills the rest, an honest peer repeats it, the attacker
     disconnects and comes back with a fresh connection and 4096 more; the
     genuine announcement, vouched for by the honest peer, is the one accepted
     at the base -- the review measured 0 here
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
BLS_SIG_SIZE = 96
HOLD_MAX = 4096          # DSL_EARLY_RESPONSES_MAX
PER_MASTERNODE = 4       # DSL_EARLY_RESPONSES_PER_MASTERNODE
SEASONING = 10 * 60      # DSL_EARLY_VOUCHER_SEASONING


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
        self.set_dash_test_params(3, 1, extra_args=[args, args, args])

    def catch_up(self, receiver, source, height, expected=None):
        with receiver.assert_debug_log(expected_msgs=expected or [], timeout=10):
            for h in range(receiver.getblockcount() + 1, height + 1):
                assert_equal(receiver.submitblock(source.getblock(source.getblockhash(h), 0)), None)
        assert_equal(receiver.getblockcount(), height)

    def flood(self, peer, epoch, count, first=0):
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
        flooder = receiver.add_p2p_connection(Quiet())
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

        # ---- phase 1: one peer floods 4096 distinct junk; the genuine one still gets in
        base, epoch = bases[0], bases[0] // EPOCH_INTERVAL
        distance = base - receiver.getblockcount()
        assert 8 <= distance <= EPOCH_INTERVAL, distance
        self.log.info(f"phase 1, epoch {epoch}, {distance} blocks early: a flood of {HOLD_MAX} distinct junk announcements is held")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (junk_protx(HOLD_MAX - 1), distance)]):
            self.flood(flooder, epoch, HOLD_MAX)

        self.log.info("the flooder's next one is dropped: it would be the weakest entry")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, dropped (hold full, every entry vouched for at least as well), peer="
                % (junk_protx(HOLD_MAX), distance)]):
            flooder.send_and_ping(msg_poseresp(epoch, junk_protx(HOLD_MAX)))

        self.log.info("a repeat brought by another peer costs nothing, and that peer joins its vouchers")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, already held, vouched for by 2 peer(s), peer=" % (junk_protx(0), distance)]):
            relay.send_and_ping(msg_poseresp(epoch, junk_protx(0)))

        self.log.info("the genuine announcement from another peer is held in a flooder's place")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one vouched for by 1 peer(s), peer=" % (proTx, distance)]):
            relay.send_and_ping(genuine[epoch])

        self.log.info("the base connects: of the full hold, exactly the genuine announcement is accepted")
        self.catch_up(receiver, miner, base, [
            "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (HOLD_MAX, epoch),
            "accepted (1 responded so far)",
        ])
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        # ---- phase 2: valid signatures over the wrong message, replayed under one masternode
        base, epoch = bases[1], bases[1] // EPOCH_INTERVAL
        self.catch_up(receiver, miner, base - 10)
        distance = 10
        others = [genuine[e] for e in sorted(genuine) if e != epoch]
        self.log.info(f"phase 2, epoch {epoch}: {PER_MASTERNODE} replayed signatures for the masternode are held, the fifth is dropped at its cap")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (proTx, distance)]):
            for other in others[:PER_MASTERNODE]:
                flooder.send_message(msg_poseresp(epoch, proTx, other.sig))
            flooder.sync_with_ping()
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, dropped (%d already held for this masternode, each vouched for at least as well), peer="
                % (proTx, distance, PER_MASTERNODE)]):
            flooder.send_and_ping(msg_poseresp(epoch, proTx, others[PER_MASTERNODE].sig))

        self.log.info("the genuine one from another peer displaces one of them")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one vouched for by 1 peer(s) for the same masternode, peer="
                % (proTx, distance)]):
            relay.send_and_ping(genuine[epoch])

        self.log.info("the base connects: four signatures checked, one accepted, three bad")
        with receiver.assert_debug_log(expected_msgs=["refused: bad signature"] * 3 + [
                "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (PER_MASTERNODE, epoch),
                "accepted (1 responded so far)"], timeout=10):
            self.catch_up(receiver, miner, base)
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        # ---- phase 3: the review's reconnect
        base, epoch = bases[2], bases[2] // EPOCH_INTERVAL
        self.catch_up(receiver, miner, base - 10)
        distance = 10
        # both peers have been connected long enough to count as seasoned; a
        # fresh connection is not
        self.bump_mocktime(SEASONING + 60)
        self.log.info(f"phase 3, epoch {epoch}: the attacker delivers the genuine announcement first and owns nothing for it")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (proTx, distance)]):
            flooder.send_and_ping(genuine[epoch])
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (junk_protx(HOLD_MAX - 2), distance)]):
            self.flood(flooder, epoch, HOLD_MAX - 1)
        self.log.info("an honest peer repeats the genuine one: it now has two vouchers")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, already held, vouched for by 2 peer(s), peer=" % (proTx, distance)]):
            relay.send_and_ping(genuine[epoch])

        self.log.info("the attacker disconnects: its vouching goes with it")
        with receiver.assert_debug_log(expected_msgs=["Cleared nodestate for peer="], timeout=10):
            flooder.peer_disconnect()
            flooder.wait_for_disconnect()
        self.wait_until(lambda: receiver.getconnectioncount() == 1, timeout=30)
        flooder_again = receiver.add_p2p_connection(Quiet())

        self.log.info("back on a fresh connection, its first junk displaces one of its own orphaned entries, not the genuine one")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one vouched for by 0 peer(s), peer="
                % (junk_protx(HOLD_MAX), distance)]):
            flooder_again.send_and_ping(msg_poseresp(epoch, junk_protx(HOLD_MAX)))
        self.log.info(f"and {HOLD_MAX - 1} more displace only its orphaned ones; once those are gone its next is dropped, "
                      "and the genuine one, vouched for by a seasoned honest peer, stands")
        # the first HOLD_MAX - 2 of these evict the remaining orphans (0 vouchers);
        # the last finds none and would be the weakest entry itself -- its
        # voucher carries the most -- so it is dropped rather than churned
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one vouched for by 0 peer(s), peer="
                % (junk_protx(2 * HOLD_MAX - 2), distance),
                "proTx=%064x arrived %d block(s) before its base block, dropped (hold full, every entry vouched for at least as well), peer="
                % (junk_protx(2 * HOLD_MAX - 1), distance)], timeout=30):
            self.flood(flooder_again, epoch, HOLD_MAX - 1, first=HOLD_MAX + 1)

        self.log.info("the base connects: the genuine announcement is the one accepted")
        self.catch_up(receiver, miner, base, [
            "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (HOLD_MAX, epoch),
            "accepted (1 responded so far)",
        ])
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLEarlyAnnouncementHoldFloodTest().main()
