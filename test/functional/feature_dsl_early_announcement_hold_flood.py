#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A genuine early announcement survives a hold that one peer has flooded.

The hold for early announcements (feature_dsl_early_announcement_hold.py)
keeps messages nobody can verify yet: the epoch's base block, and with it the
masternode list and the operator key, do not exist on this node until it
connects. So the hold's capacity is the one thing a peer can attack before
authentication -- fill it with junk, and a genuine announcement arriving after
the fill is lost exactly as it was before the hold existed. The independent
review of the hold proved that with 4096 zero-signature copies from one peer.

What the hold does about it, and what this test checks, message by message:

  - identical copies share one entry: the flood forwards the same
    announcement through every peer, and a repeat costs nothing
  - when the epoch's hold is full, the entry that gives way belongs to the
    peer holding the most of it -- never the newcomer -- so a genuine
    announcement relayed by another peer displaces one of the flooder's
  - the flooder's own next message is what gets dropped, because it could
    displace nobody but itself

The genuine announcement is a real one: captured from a registered masternode
as it announces for the epoch, delivered to a receiver that is still a full
epoch short of the base, after the flood. Once the receiver reaches the base,
that announcement -- and only it -- is accepted from the hold, and the
receiver's responded count is 1. The flood test in the review recorded 0 here.
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
BLS_SIG_SIZE = 96
HOLD_MAX = 4096  # DSL_EARLY_RESPONSES_MAX


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

    def submit_up_to(self, receiver, source, height, expected):
        with receiver.assert_debug_log(expected_msgs=expected):
            for h in range(receiver.getblockcount() + 1, height + 1):
                assert_equal(receiver.submitblock(source.getblock(source.getblockhash(h), 0)), None)
        assert_equal(receiver.getblockcount(), height)

    def run_test(self):
        miner, receiver, masternode = self.nodes
        for node in self.nodes:
            force_finish_mnsync(node)
        # the receiver hears about blocks and announcements only from this
        # test's own peers, so its distance to the base is under control
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        assert_equal(receiver.getconnectioncount(), 0)

        capture = masternode.add_p2p_connection(Capture())
        flooder = receiver.add_p2p_connection(Quiet())
        relay = receiver.add_p2p_connection(Quiet())

        # the miner and the masternode step to the next epoch base while the
        # receiver stays behind; the masternode announces on that base's tick
        start = miner.getblockcount()
        base = (start // EPOCH_INTERVAL + 1) * EPOCH_INTERVAL
        if base - start < 8:
            base += EPOCH_INTERVAL
        epoch = base // EPOCH_INTERVAL
        self.generate(miner, base - start, sync_fun=lambda: self.sync_blocks([miner, masternode]))
        assert_equal(masternode.getblockcount(), base)
        self.wait_until(lambda: any(r.nEpoch == epoch for r in capture.responses), timeout=30)
        genuine = next(r for r in capture.responses if r.nEpoch == epoch)
        distance = base - receiver.getblockcount()
        assert 8 <= distance <= EPOCH_INTERVAL, distance
        self.log.info(f"receiver at {receiver.getblockcount()}, base {base} (epoch {epoch}) is {distance} blocks away; "
                      f"genuine announcement by {genuine.proTxHash:064x} captured")

        self.log.info(f"one peer floods the hold with {HOLD_MAX} distinct junk announcements: every one is held")
        last = junk_protx(HOLD_MAX - 1)
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held, peer=" % (last, distance)]):
            for i in range(HOLD_MAX):
                flooder.send_message(msg_poseresp(epoch, junk_protx(i)))
            flooder.sync_with_ping()

        self.log.info("the flooder's next one is dropped: it holds the most, and could displace only itself")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, dropped (hold full and this peer holds the most of it, %d), peer="
                % (junk_protx(HOLD_MAX), distance, HOLD_MAX)]):
            flooder.send_and_ping(msg_poseresp(epoch, junk_protx(HOLD_MAX)))

        self.log.info("a repeat of one already held costs nothing, whichever peer brings it")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, already held, peer=" % (junk_protx(0), distance)]):
            relay.send_and_ping(msg_poseresp(epoch, junk_protx(0)))

        self.log.info("the genuine announcement arrives from another peer: held, in place of one of the flooder's")
        with receiver.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held in place of one of %d from peer="
                % (genuine.proTxHash, distance, HOLD_MAX)]):
            relay.send_and_ping(genuine)

        self.log.info("the base connects: of the full hold, exactly the genuine announcement is accepted")
        self.submit_up_to(receiver, miner, base, [
            "1 of %d held announcement(s) for epoch %d accepted once its base block connected" % (HOLD_MAX, epoch),
            "accepted (1 responded so far)",
        ])
        assert_equal(receiver.dslstatus()["respondedcount"], 1)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLEarlyAnnouncementHoldFloodTest().main()
