#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A few connections cannot crowd a lagging node's honest announcements out of the hold.

feature_dsl_early_announcement_hold_flood.py protects one genuine announcement
against one flooder. A node that has fallen behind meets a different crowd:
every honest peer delivers the SAME set of announcements -- the flood forwards
each one through every peer -- so each honest peer's share of the hold is the
whole honest set, N entries. The hold's contest makes the peer that pushed the
most give way, so k attacking connections splitting what is left of a hold of
S entries win once each holds less than an honest peer does: k > S/N - 1.

With the hold at four entries per masternode that was four connections on any
network size, and it was measured. The hold is now 28 per masternode, never
below 4096, so crowding it takes about as many connections as the peers each
honest message arrives through. This test pins that at the regtest size: the
list is under the floor, the hold is 4096, the honest set is 64, and four
attacking connections -- the number that won before -- take everything else.

The receiver is NOT whitelisted, so every connection pays the per-peer budget
at the DSL entry, and the test checks that none of them ran out: what is
measured here is the hold, not the budget. Nothing here is a masternode, so the
drain refuses every entry without a signature check, with a line naming it --
which is how the test reads which honest entries were still held at the base.

  1. honest first: three honest peers deliver the same 64 announcements, four
     attacking connections fill the rest, then push more into the full hold --
     none of it gets in, and all 64 honest entries are drained at the base
  2. honest last: the four attacking connections fill the whole hold first; the
     64 honest announcements displace attacker entries one by one, the
     attackers' further pushes are dropped, and all 64 are drained at the base
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
BLS_SIG_SIZE = 96
# DSLEarlyResponsesMax(): 28 per masternode, the list sized up to 64, never
# below 4096 -- the floor is what binds on a network with no masternodes
HOLD_MAX = 4096
HONEST = 64               # the honest set every honest peer delivers
HONEST_PEERS = 3
ATTACKERS = 4             # the number of connections that crowded the old 4 x N hold
EXTRA = 64                # what each attacker pushes into the full hold afterwards
DISTANCE = 10             # blocks the receiver is behind each base
EXHAUSTED = "per-peer budget exhausted"
DRAIN_SUMMARY = ("0 of %d held announcement(s) for epoch %d accepted once its base block connected "
                 "(0 refused for a bad signature, 0 skipped: vouched for only by peers that delivered one)")


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


MESSAGEMAP[b"poseresp"] = msg_poseresp


def honest_protx(i):
    return (0xBEEF << 240) | i


def junk_protx(attacker, i):
    # distinct per attacker and message, and none of them a masternode
    return (0xDEAD << 240) | (attacker << 32) | i


class Quiet(P2PInterface):
    """Sends only."""
    def on_poseresp(self, message):
        pass


class DSLEarlyAnnouncementHoldCrowdTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-testactivationheight=dsl@1",
            "-dip3params=2:2",
        ]]

    def log_since(self, mark):
        with open(self.nodes[0].debug_log_path, encoding="utf-8") as fh:
            fh.seek(mark)
            return fh.read()

    def deliver(self, peer, epoch, protxs):
        for protx in protxs:
            peer.send_message(msg_poseresp(epoch, protx))
        peer.sync_with_ping(timeout=120)

    def to_distance(self, base):
        node = self.nodes[0]
        self.generate(node, base - DISTANCE - node.getblockcount())
        assert_equal(node.getblockcount(), base - DISTANCE)

    def drain(self, base, epoch, mark):
        """Mine to the base; return the log from mark to the drain's summary."""
        node = self.nodes[0]
        with node.assert_debug_log(expected_msgs=[DRAIN_SUMMARY % (HOLD_MAX, epoch)], timeout=60):
            self.generate(node, base - node.getblockcount())
        log = self.log_since(mark)
        assert EXHAUSTED not in log, "a connection ran out of budget: this would measure the budget, not the hold"
        survivors = sum(1 for i in range(HONEST)
                        if "announcement by %064x for epoch %d refused: not a masternode" % (honest_protx(i), epoch) in log)
        return survivors

    def run_test(self):
        node = self.nodes[0]
        force_finish_mnsync(node)
        self.generate(node, EPOCH_INTERVAL)

        # Every connection is made here and earns from here: the receiver
        # connects more than two epochs of blocks before the first hold, so
        # each bucket is at its cap and holds twice what one phase spends.
        honest = [node.add_p2p_connection(Quiet()) for _ in range(HONEST_PEERS)]
        attackers = [node.add_p2p_connection(Quiet()) for _ in range(ATTACKERS)]
        fill = (HOLD_MAX - HONEST) // ATTACKERS
        assert_equal(fill * ATTACKERS + HONEST, HOLD_MAX)

        # ---- phase 1: honest first, then the crowd
        base = (node.getblockcount() // EPOCH_INTERVAL + 3) * EPOCH_INTERVAL
        epoch = base // EPOCH_INTERVAL
        self.to_distance(base)
        mark = node.debug_log_bytes()
        self.log.info(f"phase 1, epoch {epoch}: {HONEST_PEERS} honest peers deliver the same {HONEST} announcements")
        for peer in honest:
            self.deliver(peer, epoch, [honest_protx(i) for i in range(HONEST)])
        self.log.info(f"{ATTACKERS} attacking connections fill the other {HOLD_MAX - HONEST} entries, {fill} each")
        for a, peer in enumerate(attackers):
            self.deliver(peer, epoch, [junk_protx(a, i) for i in range(fill)])
        log = self.log_since(mark)
        assert_equal(log.count("before its base block, held, peer="), HOLD_MAX)
        assert_equal(log.count("already held, vouched for by"), HONEST * (HONEST_PEERS - 1))

        self.log.info(f"each pushes {EXTRA} more into the full hold: nothing honest gives way")
        mark_extra = node.debug_log_bytes()
        for a, peer in enumerate(attackers):
            self.deliver(peer, epoch, [junk_protx(a, fill + i) for i in range(EXTRA)])
        log = self.log_since(mark_extra)
        assert_equal(log.count("dropped (hold full, no entry pushed here by fewer)"), ATTACKERS * EXTRA)
        assert_equal(log.count("held in place of"), 0)

        self.log.info("the base connects: every honest announcement was still held")
        assert_equal(self.drain(base, epoch, mark), HONEST)

        # ---- phase 2: the crowd first, then honest
        base += EPOCH_INTERVAL
        epoch = base // EPOCH_INTERVAL
        self.to_distance(base)
        mark = node.debug_log_bytes()
        whole = HOLD_MAX // ATTACKERS
        self.log.info(f"phase 2, epoch {epoch}: {ATTACKERS} attacking connections fill the whole hold, {whole} each")
        for a, peer in enumerate(attackers):
            self.deliver(peer, epoch, [junk_protx(a, i) for i in range(whole)])
        assert_equal(self.log_since(mark).count("before its base block, held, peer="), HOLD_MAX)

        self.log.info(f"the {HONEST} honest announcements arrive last, and each displaces an attacker's entry")
        mark_honest = node.debug_log_bytes()
        for peer in honest:
            self.deliver(peer, epoch, [honest_protx(i) for i in range(HONEST)])
        log = self.log_since(mark_honest)
        assert_equal(log.count("held in place of one whose lightest voucher pushed"), HONEST)
        assert_equal(log.count("already held, vouched for by"), HONEST * (HONEST_PEERS - 1))

        self.log.info(f"each attacker pushes {EXTRA} more: dropped, nothing honest gives way")
        mark_extra = node.debug_log_bytes()
        for a, peer in enumerate(attackers):
            self.deliver(peer, epoch, [junk_protx(a, whole + i) for i in range(EXTRA)])
        log = self.log_since(mark_extra)
        assert_equal(log.count("dropped (hold full, no entry pushed here by fewer)"), ATTACKERS * EXTRA)
        assert_equal(log.count("held in place of"), 0)

        self.log.info("the base connects: every honest announcement was still held")
        assert_equal(self.drain(base, epoch, mark), HONEST)

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLEarlyAnnouncementHoldCrowdTest().main()
