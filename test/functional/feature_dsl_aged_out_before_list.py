#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A Sentinel message for an epoch older than the retained window is refused before the list is built.

The handlers refuse an announcement (ProcessResponse) or a report (the report store) whose epoch has aged out of the
retained window, but the node used to build the masternode list of that epoch's base block first, as the handler's
argument: for an old epoch a replay of diffs back to a snapshot, paid for one small message. The window is now asked
first, of the manager's and the store's own epoch, with the same expression.

On one node with the Sentinel layer active from block 1 and the cached chain mined on into epoch nine, so that epoch
one has aged out, an announcement and a report for that epoch are refused before the list build (the new log line),
and the handlers' own refusal lines do not appear. An announcement for the oldest epoch still in the window passes the check and
reaches the handler. The negative control runs this on the base, where the handlers' lines appear instead.
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import force_finish_mnsync

EPOCH_INTERVAL = 24
KEEP_EPOCHS = 8
BLS_SIG_SIZE = 96


class msg_poseresp:
    """CPoSeServiceResponse on the wire: nEpoch, proTxHash, a basic-scheme BLS
    signature."""
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


class msg_posereport:
    """CPoSeServiceReport on the wire: nEpoch, target, sentinel, status, sig."""
    __slots__ = ("nEpoch", "target", "sentinel", "status", "sig")
    msgtype = b"posereport"

    def __init__(self, nEpoch=0, target=0, sentinel=0, status=2):
        self.nEpoch = nEpoch
        self.target = target
        self.sentinel = sentinel
        self.status = status
        self.sig = b"\x00" * BLS_SIG_SIZE

    def deserialize(self, f):
        self.nEpoch = struct.unpack("<I", f.read(4))[0]
        self.target = int.from_bytes(f.read(32), "little")
        self.sentinel = int.from_bytes(f.read(32), "little")
        self.status = struct.unpack("<B", f.read(1))[0]
        self.sig = f.read(BLS_SIG_SIZE)

    def serialize(self):
        return (struct.pack("<I", self.nEpoch) + ser_uint256(self.target) + ser_uint256(self.sentinel)
                + struct.pack("<B", self.status) + self.sig)

    def __repr__(self):
        return "msg_posereport(nEpoch=%d)" % self.nEpoch


class DSLAgedOutBeforeList(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # noban: the per-peer DSL message budget does not apply
        self.extra_args = [["-testactivationheight=dsl@1", "-whitelist=noban@127.0.0.1"]]

    def run_test(self):
        node = self.nodes[0]
        force_finish_mnsync(node)
        # The DSL tick moves the manager and the store to the tip's epoch on a block, once the node is synced. The
        # cached chain ends inside epoch 8, where nothing has aged out yet: mine to the first block of epoch 9, and wait
        # for the tick, which runs on the validation queue.
        self.generate(node, max(1, (KEEP_EPOCHS + 1) * EPOCH_INTERVAL - node.getblockcount()))
        node.syncwithvalidationinterfacequeue()
        current = node.getblockcount() // EPOCH_INTERVAL
        assert current >= KEEP_EPOCHS + 1, current
        aged = current - KEEP_EPOCHS
        inside = aged + 1
        peer = node.add_p2p_connection(P2PInterface())

        self.log.info(f"An announcement for epoch {aged} (current {current}) is refused before the list build")
        with node.assert_debug_log(expected_msgs=[f"for epoch {aged} refused before the list build: aged out"],
                                   unexpected_msgs=[f"for epoch {aged} refused: aged out"]):
            peer.send_message(msg_poseresp(aged, 0x11))
            peer.sync_with_ping()

        self.log.info(f"A report for epoch {aged} is refused before the list build")
        with node.assert_debug_log(expected_msgs=[f"for epoch {aged} refused before the list build: epoch aged out"],
                                   unexpected_msgs=[f"for epoch {aged} refused: epoch aged out"]):
            peer.send_message(msg_posereport(aged, 0x22, 0x33))
            peer.sync_with_ping()

        self.log.info(f"An announcement for epoch {inside}, the oldest in the window, still reaches the handler")
        with node.assert_debug_log(expected_msgs=[f"poseresp epoch={inside} proTx={0x44:064x} accepted=0"],
                                   unexpected_msgs=["refused before the list build"]):
            peer.send_message(msg_poseresp(inside, 0x44))
            peer.sync_with_ping()


if __name__ == '__main__':
    DSLAgedOutBeforeList().main()
