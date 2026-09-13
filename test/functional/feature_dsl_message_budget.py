#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""One peer cannot spend this node's CPU on DSL gossip without limit.

The three DSL messages (poseresp, posereport, posechal) had no peer accounting
of any kind: no rate limit, and no path to Misbehaving either, because
ProcessDSLMessage returns void and so cannot be scored through
ProcessPeerMsgRet. A failed verification is never remembered -- the dedup maps
record only ACCEPTED items -- so one real proTxHash carrying a wrong signature
passes the filter for as long as the sender keeps sending it, and every copy
costs a BLS pairing (2.49 ms measured on this tree). DSL ingest runs on the
single message thread, so that is a thread an attacker can hold.

The fix is a budget per connection, charged at the entry of ProcessDSLMessage
before any deserialization, and earned per connected block. What this test has
to prove is both halves, and the honest half matters more: a budget too tight
would throttle honest gossip, and that failure looks exactly like the lost epoch
the Sentinel layer has been chasing since #207.

  1. a bare connection starts with nothing, so dropping the connection and
     coming back buys nothing either: anything handed out up front would be
     handed out again on every reconnect
  2. a connection earns one block's share per block, from the moment it was
     made -- a quiet, long-lived connection meets its first burst with a full
     bucket, not an empty one
  3. an ordinary honest epoch passes untouched. One peer delivers
     N + N * nDSLSentinelCount messages over an epoch: every masternode
     announces once, EmitReports files a report for every assigned target
     whether it answered or not, and the flood forwards each message once per
     peer. That is not a rare worst case: measured on the devnet, one peer's
     epoch was 151 announcements and 1064 reports while the commitments for the
     same window recorded nobody missing. This test takes the devnet's figure,
     152 * 8 = 1216 in a single epoch with no block mined in between
  4. the budget binds at the cap and not before, and one block returns exactly
     one block's share
  5. the budget is one per connection and not one per message type
  6. a verified masternode is handed an epoch's worth up front -- quorum links
     reopen every DKG cycle and must not drop the burst that follows -- but at
     most DSL_MSG_BUDGET_GRANTS_PER_IDENTITY connections of one identity per
     epoch, so reconnecting as a masternode buys nothing past the honest number
     of links; a new epoch grants again
  7. a message that cannot be read is scored, and ten of them are a
     discouragement. A message that reads but is rejected is not scored: after
     a reorg the epoch's base changes and honest announcements in flight are
     signed against the old one, so they fail legitimately
  8. the budget covers everything that reaches the DSL entry, not only what
     ends in a signature check: a posechal from a connection with nothing
     earned is dropped unread, so ten unreadable ones cost it nothing and
     score nothing, and an early announcement from it never reaches the hold.
     And a connection this node opened itself is handed an epoch's worth up
     front, once -- a block producer attaches the epoch's commitment only from
     its own pool, and must not meet the report burst on its own outbound
     links with nothing earned -- while a block-relay-only link never reaches
     the budget at all: DSL on it is a protocol violation. That credit belongs
     to the address, at most DSL_MSG_BUDGET_GRANTS_PER_OUTBOUND_ADDR
     connections an epoch: a manual connection comes back on its own whenever
     the other side drops it, so per connection it would be credit per
     reconnect

Closes F-2026-131. The other half of R-08, the early hold, is pinned by
feature_dsl_early_announcement_hold_flood.py and
feature_dsl_early_announcement_hold_crowd.py.
"""

import struct

from test_framework.messages import ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

EPOCH_INTERVAL = 24        # Consensus::Params::nDSLEpochInterval
SENTINELS = 7              # Consensus::Params::nDSLSentinelCount
MIN_MNS = 64               # DSL_MSG_BUDGET_MIN_MNS
CAP_EPOCHS = 4             # DSL_MSG_BUDGET_CAP_EPOCHS
REFILL_EPOCHS = 2          # DSL_MSG_BUDGET_REFILL_EPOCHS
# DSL_MSG_BUDGET_GRANTS_PER_IDENTITY: MAX_VERIFIED_INBOUND_PER_PROTX (3) verified
# inbound connections of one identity, plus the one outbound it can hold to us
GRANTS_PER_IDENTITY = 3 + 1
GRANTS_PER_OUTBOUND_ADDR = 2  # DSL_MSG_BUDGET_GRANTS_PER_OUTBOUND_ADDR
# This network has no masternodes, so the floor is what sizes the budget.
CEILING = MIN_MNS * (1 + SENTINELS)                           # honest messages per peer per epoch
BUDGET = CAP_EPOCHS * CEILING                                 # what a connection may hold at once
PER_BLOCK = REFILL_EPOCHS * CEILING // EPOCH_INTERVAL         # whole messages one block returns
BLOCKS_TO_CAP = CAP_EPOCHS * EPOCH_INTERVAL // REFILL_EPOCHS  # blocks from empty to the cap
# The devnet the layer actually runs on: 152 masternodes, each announcing once
# and reportable by SENTINELS sentinels. Deliberately not derived from the above.
DEVNET_HONEST_EPOCH = 152 * (1 + SENTINELS)  # 1216

UNREADABLE_SCORE = 10      # DSL_MSG_MISBEHAVING_UNREADABLE
DISCOURAGEMENT = 100       # DISCOURAGEMENT_THRESHOLD
BLS_SIG_SIZE = 96

EXHAUSTED = "per-peer budget exhausted"


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


class msg_poseresp_truncated:
    """Four bytes short of an epoch number: nothing can read this."""
    msgtype = b"poseresp"

    def serialize(self):
        return b"\x01\x02"

    def __repr__(self):
        return "msg_poseresp_truncated()"


class msg_posechal:
    """CPoSeServiceChallenge on the wire: nEpoch alone."""
    msgtype = b"posechal"

    def __init__(self, nEpoch=0):
        self.nEpoch = nEpoch

    def serialize(self):
        return struct.pack("<I", self.nEpoch)

    def __repr__(self):
        return "msg_posechal(nEpoch=%d)" % self.nEpoch


class msg_posechal_truncated:
    """Two bytes of a four-byte epoch number: nothing can read this."""
    msgtype = b"posechal"

    def serialize(self):
        return b"\x01\x02"

    def __repr__(self):
        return "msg_posechal_truncated()"


MESSAGEMAP[b"poseresp"] = msg_poseresp
MESSAGEMAP[b"posereport"] = msg_posereport


def junk_protx(i):
    # distinct per message, and none of them a masternode
    return (0xDEAD << 240) | i


class Quiet(P2PInterface):
    """Sends only."""
    def on_poseresp(self, message):
        pass

    def on_posereport(self, message):
        pass


class DSLMessageBudgetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-testactivationheight=dsl@1",
            # the DSL path sits in the producer's DIP3-active section, as on
            # every network that runs the layer
            "-dip3params=2:2",
            "-deprecatedrpc=banscore",
        ]]

    # -- helpers ----------------------------------------------------------
    def log_since(self, mark):
        with open(self.nodes[0].debug_log_path, encoding="utf-8") as fh:
            fh.seek(mark)
            return fh.read()

    def epoch(self):
        return self.nodes[0].getblockcount() // EPOCH_INTERVAL

    def send(self, peer, first, count):
        """Send `count` distinct announcements for the current epoch; return
        (how many the node read, whether it reported a drop)."""
        node = self.nodes[0]
        epoch = self.epoch()
        mark = node.debug_log_bytes()
        for i in range(first, first + count):
            peer.send_message(msg_poseresp(epoch, junk_protx(i)))
        peer.sync_with_ping()
        log = self.log_since(mark)
        return log.count("DSL -- poseresp epoch=%d " % epoch), EXHAUSTED in log

    def connect(self):
        """A new inbound connection and the id the node gave it -- ids rise with
        every connection, so the one just added is the highest."""
        peer = self.nodes[0].add_p2p_connection(Quiet())
        return peer, max(info["id"] for info in self.nodes[0].getpeerinfo())

    def outbound(self):
        """A new full outbound connection from the node, and its id. One at a
        time: the slot index is reused, so the previous one must be gone."""
        peer = self.nodes[0].add_outbound_p2p_connection(Quiet(), p2p_idx=0, connection_type="outbound-full-relay")
        return peer, max(info["id"] for info in self.nodes[0].getpeerinfo())

    def disconnect(self, peer, peer_id):
        peer.peer_disconnect()
        peer.wait_for_disconnect()
        self.wait_until(lambda: all(info["id"] != peer_id for info in self.nodes[0].getpeerinfo()))

    def banscore(self, peer_id):
        for info in self.nodes[0].getpeerinfo():
            if info["id"] == peer_id:
                return info["banscore"]
        raise AssertionError("peer %d is gone from getpeerinfo" % peer_id)

    # -- the test ---------------------------------------------------------
    def run_test(self):
        node = self.nodes[0]

        # A tip on an epoch boundary, so the base block of the epoch the peers
        # name exists and their messages take the ordinary path rather than the
        # hold for early announcements. Nothing below depends on where they end
        # up: the budget is charged before any of that.
        self.generate(node, 2 * EPOCH_INTERVAL)
        assert_equal(node.getblockcount() % EPOCH_INTERVAL, 0)

        self.log.info("1. A bare connection starts with nothing")
        bare, bare_id = self.connect()
        assert_equal(self.send(bare, 0, 20), (0, True))
        self.log.info("   and coming back on a fresh connection buys nothing either")
        self.disconnect(bare, bare_id)
        peer, peer_id = self.connect()
        assert_equal(self.send(peer, 20, 20), (0, True))

        # two connections that will say nothing until much later
        quiet, _ = self.connect()
        reporter, _ = self.connect()

        self.log.info("2. One block earns exactly one block's share (%d)", PER_BLOCK)
        self.generate(node, 1)
        assert_equal(self.send(peer, 100, PER_BLOCK + 20), (PER_BLOCK, True))

        self.log.info("   %d blocks earn the cap (%d)", BLOCKS_TO_CAP, BUDGET)
        self.generate(node, BLOCKS_TO_CAP)

        self.log.info("3. An ordinary honest devnet epoch from one peer (%d messages) is not throttled", DEVNET_HONEST_EPOCH)
        assert_equal(self.send(peer, 1000, DEVNET_HONEST_EPOCH), (DEVNET_HONEST_EPOCH, False))

        self.log.info("4. The rest of the cap (%d more) is spent without a drop, and nothing past it is read", BUDGET - DEVNET_HONEST_EPOCH)
        assert_equal(self.send(peer, 3000, BUDGET - DEVNET_HONEST_EPOCH), (BUDGET - DEVNET_HONEST_EPOCH, False))
        assert_equal(self.send(peer, 5000, 100), (0, True))
        self.generate(node, 1)
        assert_equal(self.send(peer, 6000, PER_BLOCK + 20), (PER_BLOCK, True))

        self.log.info("2'. A connection that stayed quiet all along earned from the moment it was made: its first burst is a full cap")
        assert_equal(self.send(quiet, 7000, BUDGET), (BUDGET, False))

        self.log.info("5. One budget per connection, shared by every DSL message type")
        epoch = self.epoch()
        mark = node.debug_log_bytes()
        for who in (peer, reporter):   # peer has spent its budget, reporter has spent nothing
            who.send_message(msg_posereport(epoch, junk_protx(1), junk_protx(2)))
            who.sync_with_ping()
        assert_equal(self.log_since(mark).count("DSL -- posereport epoch=%d " % epoch), 1)

        self.log.info("6. A verified masternode is handed an epoch's worth (%d) up front", CEILING)
        # any valid basic-scheme operator key will do: the regtest mnauth
        # override only hashes it, and `bls generate` would need a wallet
        public_key = "b7e9a1450e949d950ba00b0301bc90564ee0a386dff42cff7927943e04f6d80ab9d918ef7970b63fb67c601681101e74"
        identity = "ab" * 32
        for n in range(1, GRANTS_PER_IDENTITY + 1):
            mn, mn_id = self.connect()
            assert node.mnauth(mn_id, identity, public_key)
            assert_equal(self.send(mn, 10000 * n, CEILING + 20), (CEILING, True))
            self.log.info("   connection %d of the identity was granted, and it reconnects", n)
            self.disconnect(mn, mn_id)
        self.log.info("   past %d connections in an epoch, the identity's next connection is granted nothing", GRANTS_PER_IDENTITY)
        spent_mn, spent_mn_id = self.connect()
        assert node.mnauth(spent_mn_id, identity, public_key)
        assert_equal(self.send(spent_mn, 60000, 20), (0, True))
        self.log.info("   another identity is granted its own")
        other, other_id = self.connect()
        assert node.mnauth(other_id, "cd" * 32, public_key)
        assert_equal(self.send(other, 70000, CEILING + 20), (CEILING, True))
        self.log.info("   and the next epoch grants the first identity again")
        self.generate(node, EPOCH_INTERVAL)
        again, again_id = self.connect()
        assert node.mnauth(again_id, identity, public_key)
        assert_equal(self.send(again, 80000, CEILING + 20), (CEILING, True))

        self.log.info("7. A message that reads but is rejected is never scored")
        assert_equal(self.banscore(peer_id), 0)

        self.log.info("   a message that cannot be read is scored %d, and ten are a discouragement", UNREADABLE_SCORE)
        junk, junk_id = self.connect()
        self.generate(node, 1)   # something earned, or its messages would be dropped unread and never scored
        for strike in range(1, DISCOURAGEMENT // UNREADABLE_SCORE):
            junk.send_message(msg_poseresp_truncated())
            junk.sync_with_ping()
            assert_equal(self.banscore(junk_id), strike * UNREADABLE_SCORE)
        with node.assert_debug_log(["DISCOURAGE THRESHOLD EXCEEDED"]):
            junk.send_message(msg_poseresp_truncated())
            junk.wait_for_disconnect()

        self.log.info("   the peer that only sent rejected-but-readable messages is still connected, on banscore 0")
        assert_equal(self.banscore(peer_id), 0)

        # A tip part-way into an epoch, so that an announcement for the next one
        # is early -- the path that ends in the hold, not in a signature check.
        # Every connection below is made at this tip and nothing is mined after
        # it, so none of them has earned anything.
        mid_epoch = EPOCH_INTERVAL // 2
        to_mid_epoch = mid_epoch - node.getblockcount() % EPOCH_INTERVAL   # may be negative
        self.generate(node, to_mid_epoch + EPOCH_INTERVAL)                   # so one whole epoch more
        assert_equal(node.getblockcount() % EPOCH_INTERVAL, mid_epoch)

        self.log.info("8. A posechal from a connection with nothing earned is dropped unread")
        chal, chal_id = self.connect()
        mark = node.debug_log_bytes()
        chal.send_and_ping(msg_posechal(self.epoch()))
        assert EXHAUSTED in self.log_since(mark)
        self.log.info("   so ten unreadable ones score nothing and the connection stays")
        for _ in range(DISCOURAGEMENT // UNREADABLE_SCORE):
            chal.send_message(msg_posechal_truncated())
        chal.sync_with_ping()
        assert_equal(self.banscore(chal_id), 0)

        self.log.info("   an early announcement from a connection with nothing earned never reaches the hold")
        early, _ = self.connect()
        next_epoch = self.epoch() + 1
        mark = node.debug_log_bytes()
        early.send_and_ping(msg_poseresp(next_epoch, junk_protx(90000)))
        log = self.log_since(mark)
        assert EXHAUSTED in log
        assert "before its base block" not in log

        self.log.info("   a full outbound connection this node opened is handed an epoch's worth (%d) up front, once", CEILING)
        outbound, outbound_id = self.outbound()
        assert_equal(self.send(outbound, 100000, CEILING + 20), (CEILING, True))
        self.log.info("   a block-relay-only link never reaches the budget: DSL on it is a protocol violation, and it is disconnected")
        block_only = node.add_outbound_p2p_connection(Quiet(), p2p_idx=1, connection_type="block-relay-only")
        with node.assert_debug_log(expected_msgs=["poseresp sent in violation of protocol"],
                                   unexpected_msgs=["DSL -- poseresp epoch="]):
            block_only.send_message(msg_poseresp(self.epoch(), junk_protx(110000)))
            block_only.wait_for_disconnect()

        # A manual connection is re-opened by this node whenever the other side
        # drops it, so the other side sets how often it comes back: the credit
        # has to belong to the address, not to the connection. Every outbound
        # connection here goes to 127.0.0.1, which is one address.
        self.log.info("   the credit belongs to the address: a second connection to it in the epoch is granted too (%d of %d)",
                      2, GRANTS_PER_OUTBOUND_ADDR)
        self.disconnect(outbound, outbound_id)
        outbound, outbound_id = self.outbound()
        assert_equal(self.send(outbound, 120000, CEILING + 20), (CEILING, True))
        self.log.info("   and the next one in the same epoch is granted nothing, however it came back")
        self.disconnect(outbound, outbound_id)
        outbound, outbound_id = self.outbound()
        assert_equal(self.send(outbound, 130000, 20), (0, True))
        self.log.info("   the next epoch grants the address again")
        self.disconnect(outbound, outbound_id)
        self.generate(node, EPOCH_INTERVAL)
        outbound, _ = self.outbound()
        assert_equal(self.send(outbound, 140000, CEILING + 20), (CEILING, True))


if __name__ == "__main__":
    DSLMessageBudgetTest().main()
