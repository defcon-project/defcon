#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A Sentinel announcement that outruns its epoch's base block is held, not lost.

A masternode announces its liveness on the tick of the block that opens the
epoch, and the flood forwards each copy once. A peer that has not connected
that base block yet cannot validate the announcement, and rejecting it there
loses it for the epoch: no re-request exists, and the sentinel then reports the
announcer MISSED on evidence it never got to see.

The hold used to cover exactly one block. Measured on 2026-09-11: the first
node to reach an epoch base announced while five of its six peers were still
5-10 blocks behind a 24-block burst, and every one of them dropped that
announcement as accepted=0 -- indistinguishable in the log from a duplicate --
and reported the announcer MISSED. On a live network the same gap opens
whenever a peer's block arrival lags, which this devnet measures at p99 82-297
seconds.

This test drives the receiver directly over P2P with a crafted announcement,
so the distance to the base block is exact rather than raced:

  - several blocks early: held (the case that used to be lost)
  - one block early:      held (the case that always was; the regression guard)
  - more than an epoch early: dropped (the bound on memory and on trust)

then mines to the base and checks that every held announcement was drained
into ProcessResponse -- which refuses them, because the signature is empty and
the proTxHash names no masternode, and says so in the log. Refusal at the
drain is the proof: the message reached the pool's validation instead of dying
on the wire.

The last phase is the hold on a node that is not masternode-synced. Such a
node connects blocks but its DSL tick returns before draining anything, so the
tick cannot be what bounds the hold: left to the tick, every epoch boundary
the tip crossed would key one more vector of up to 4096 entries. The bound is
kept at the insert instead -- a key that is neither the tip's epoch nor the
next is stale and discarded, and the log says how many went. The phase holds
three announcements, crosses two boundaries with the tick skipped (by the
node's own word), and reads the discard of exactly those three; then it lets
the sync finish and checks the tip's own epoch still drains what it held.
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
BLS_SIG_SIZE = 96


class msg_poseresp:
    """CPoSeServiceResponse on the wire: nEpoch, proTxHash, a basic-scheme BLS
    signature. An all-zero signature deserialises as the invalid signature
    (bls.h SetBytes: all zeros means Reset) and is refused at validation, which
    is exactly what this test asserts on."""
    __slots__ = ("nEpoch", "proTxHash", "sig")
    msgtype = b"poseresp"

    def __init__(self, nEpoch, proTxHash):
        self.nEpoch = nEpoch
        self.proTxHash = proTxHash
        self.sig = b"\x00" * BLS_SIG_SIZE

    def deserialize(self, f):
        self.nEpoch = struct.unpack("<I", f.read(4))[0]
        self.proTxHash = int.from_bytes(f.read(32), "little")
        self.sig = f.read(BLS_SIG_SIZE)

    def serialize(self):
        return struct.pack("<I", self.nEpoch) + ser_uint256(self.proTxHash) + self.sig

    def __repr__(self):
        return "msg_poseresp(nEpoch=%d, proTxHash=%064x)" % (self.nEpoch, self.proTxHash)


def fake_protx(tag):
    # a distinct, recognisable proTxHash per case, built from one repeated byte
    # so its display form is the same whichever way the bytes are read; none of
    # them is a masternode
    return int(("%02x" % tag) * 32, 16)


class DSLEarlyAnnouncementHoldTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-testactivationheight=dsl@1"]]

    def send_announcement(self, peer, epoch, tag):
        proTx = fake_protx(tag)
        peer.send_and_ping(msg_poseresp(epoch, proTx))
        return proTx

    def run_test(self):
        node = self.nodes[0]
        force_finish_mnsync(node)

        # Wherever the cached chain leaves us, step to a point mid-epoch that is
        # several blocks short of the next base, so every distance below is a
        # known number rather than an accident of the cache height.
        tip = node.getblockcount()
        next_base = (tip // EPOCH_INTERVAL + 1) * EPOCH_INTERVAL
        if next_base - tip < 8:
            self.generate(node, next_base - tip)
            tip = node.getblockcount()
            next_base = (tip // EPOCH_INTERVAL + 1) * EPOCH_INTERVAL
        next_epoch = next_base // EPOCH_INTERVAL
        far_early = next_base - tip
        assert far_early >= 8, "the setup should sit well short of the next base, got %d" % far_early
        self.log.info(f"tip {tip}, next base {next_base} (epoch {next_epoch}), {far_early} blocks away")

        peer = node.add_p2p_connection(P2PInterface())

        self.log.info(f"{far_early} blocks early: held (the case that used to be lost)")
        with node.assert_debug_log(expected_msgs=["arrived %d block(s) before its base block, held" % far_early]):
            far = self.send_announcement(peer, next_epoch, 0x11)

        self.log.info("more than an epoch early: dropped, the bound holds")
        beyond = fake_protx(0x22)
        with node.assert_debug_log(expected_msgs=["poseresp epoch=%d proTx=%064x accepted=0" % (next_epoch + 1, beyond)],
                                   unexpected_msgs=["proTx=%064x arrived" % beyond]):
            self.send_announcement(peer, next_epoch + 1, 0x22)

        self.log.info("one block early: held, as it always was")
        self.generate(node, next_base - 1 - node.getblockcount())
        assert_equal(node.getblockcount(), next_base - 1)
        with node.assert_debug_log(expected_msgs=["arrived 1 block(s) before its base block, held"]):
            near = self.send_announcement(peer, next_epoch, 0x33)

        self.log.info("the base block connects: both held announcements are drained into validation")
        # ProcessResponse refuses each one -- the proTxHash names no masternode of
        # the epoch's list -- and the refusal is what proves the drain delivered it
        with node.assert_debug_log(expected_msgs=[
                "0 of 2 held announcement(s) for epoch %d accepted once its base block connected" % next_epoch,
                "announcement by %064x for epoch %d refused" % (far, next_epoch),
                "announcement by %064x for epoch %d refused" % (near, next_epoch)]):
            self.generate(node, 1)
            assert_equal(node.getblockcount(), next_base)

        self.log.info("an unsynced node connects blocks and never ticks: stale holds are discarded at the next insert")
        node.mnsync("reset")
        assert_equal(node.mnsync("status")["IsBlockchainSynced"], False)
        stale_epoch = next_epoch + 1
        stale_base = stale_epoch * EPOCH_INTERVAL
        with node.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held" % (fake_protx(0x66), EPOCH_INTERVAL)]):
            for tag in (0x44, 0x55, 0x66):
                self.send_announcement(peer, stale_epoch, tag)

        self.log.info("the tip crosses that epoch's base with no tick draining, by the node's own word")
        with node.assert_debug_log(expected_msgs=["tick at height %d skipped, masternode sync not finished" % stale_base],
                                   unexpected_msgs=["held announcement(s) for epoch %d accepted" % stale_epoch]):
            self.generate(node, EPOCH_INTERVAL)
        assert_equal(node.getblockcount(), stale_base)

        self.log.info("an insert with the tip in that epoch keeps its key: it is the tip's own, and a tick may still come")
        with node.assert_debug_log(expected_msgs=[
                "proTx=%064x arrived %d block(s) before its base block, held" % (fake_protx(0x77), EPOCH_INTERVAL)],
                                   unexpected_msgs=["discarded, stale before any tick drained them"]):
            self.send_announcement(peer, stale_epoch + 1, 0x77)

        self.log.info("one epoch further, the next insert discards exactly the three that no tick ever drained")
        with node.assert_debug_log(expected_msgs=["tick at height %d skipped, masternode sync not finished" % (stale_base + EPOCH_INTERVAL)]):
            self.generate(node, EPOCH_INTERVAL)
        with node.assert_debug_log(expected_msgs=[
                "3 held announcement(s) for epoch %d discarded, stale before any tick drained them" % stale_epoch,
                "proTx=%064x arrived %d block(s) before its base block, held" % (fake_protx(0x88), EPOCH_INTERVAL)]):
            self.send_announcement(peer, stale_epoch + 2, 0x88)

        self.log.info("the sync finishes: the tip's own epoch still drains what it held")
        force_finish_mnsync(node)
        with node.assert_debug_log(expected_msgs=[
                "0 of 1 held announcement(s) for epoch %d accepted once its base block connected" % (stale_epoch + 1),
                "announcement by %064x for epoch %d refused" % (fake_protx(0x77), stale_epoch + 1)]):
            self.generate(node, 1)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLEarlyAnnouncementHoldTest().main()
