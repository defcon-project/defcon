#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""An MNAUTH that arrives before blockchain sync is held, not lost (dash#7562).

A masternode sends MNAUTH once per connection, at VERACK. The receiving node
cannot check it before its masternode list is usable, and used to drop it, so a
connection opened during that window stayed unverified for its whole life. Every
MNAUTH here is the real one a masternode signs during the handshake; nothing is
faked with the mnauth RPC, which would not exercise this path at all.

1. Control: a connection opened after sync is verified at once.
2. A connection opened before sync is verified once sync finishes -- the same
   connection, no reconnect -- and the held message is processed exactly once.
3. A connection that goes away before sync takes its held MNAUTH with it.
4. Holding buys time, not trust: a message of the wrong size is not held; one of
   the right size that cannot be read (a non-canonical BLS encoding, which throws)
   is not held either, and the node keeps running; a readable one with an invalid
   signature is judged after sync by the ordinary handler, with its ordinary penalty.
"""

from test_framework.messages import NODE_BLOOM, msg_generic
from test_framework.p2p import P2P_SERVICES, P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync, p2p_port

HELD = "MNAUTH received before blockchain sync, held until it finishes"
PROCESSED = "processing the MNAUTH held until blockchain sync"
UNREADABLE = "MNAUTH received before blockchain sync is unreadable"


class MNAuthBeforeSyncTest(DashTestFramework):
    def set_test_params(self):
        # node0 controller, node1 the plain node whose sync is reset, node2 the masternode
        self.set_dash_test_params(3, 1, extra_args=[[], ["-debug=net", "-debug=netconn"], []])

    def mn_peer(self):
        """The plain node's peer entry for its connection to the masternode, or None."""
        addr = "127.0.0.1:%d" % p2p_port(self.mn.nodeIdx)
        peers = [p for p in self.plain.getpeerinfo() if p["addr"] == addr]
        assert len(peers) <= 1
        return peers[0] if peers else None

    def connect_to_mn(self, expect_msgs):
        with self.plain.assert_debug_log(expect_msgs, timeout=20):
            self.connect_nodes(self.plain.index, self.mn.nodeIdx)
        self.wait_until(lambda: self.mn_peer() is not None)
        return self.mn_peer()

    def disconnect_from_mn(self):
        self.disconnect_nodes(self.plain.index, self.mn.nodeIdx)
        self.wait_until(lambda: self.mn_peer() is None)

    def log_since(self, pos):
        with open(self.plain.debug_log_path, encoding="utf-8") as dl:
            dl.seek(pos)
            return dl.read()

    def let_message_loop_run(self, rounds=5):
        """Round trips on every connection; each is answered from the message loop."""
        for _ in range(rounds):
            self.plain.ping()
            self.wait_until(lambda: all("pingwait" not in p for p in self.plain.getpeerinfo()), timeout=20)

    def reset_sync(self):
        # The controller link keeps the peer count above zero: a 0 <-> non-0
        # transition re-runs the sync on its own and would muddy the window.
        assert any(p["addr"] == "127.0.0.1:%d" % p2p_port(0) for p in self.plain.getpeerinfo())
        self.plain.mnsync("reset")
        assert_equal(self.plain.mnsync("status")["IsBlockchainSynced"], False)

    def run_test(self):
        self.plain = self.nodes[1]
        self.mn = self.mninfo[0]
        protx = self.mn.proTxHash
        if self.mn_peer() is not None:
            self.disconnect_from_mn()

        self.log.info("1. control: connected after sync, verified at once")
        assert self.plain.mnsync("status")["IsBlockchainSynced"]
        self.connect_to_mn(["received: mnauth"])
        self.wait_until(lambda: self.mn_peer().get("verified_proregtx_hash") == protx, timeout=10)
        self.disconnect_from_mn()

        self.log.info("2. connected before sync: the MNAUTH is held, and the same connection verifies when sync finishes")
        self.reset_sync()
        peer = self.connect_to_mn(["received: mnauth"])
        peer_id = peer["id"]
        # the handshake is done and the MNAUTH is in, but nothing can check it yet
        assert_equal(self.plain.mnsync("status")["IsBlockchainSynced"], False)
        assert "verified_proregtx_hash" not in self.mn_peer()
        log_start = self.plain.debug_log_bytes()
        # the trailing newline keeps peer=3 from matching peer=31
        with self.plain.assert_debug_log([f"{PROCESSED}, peer={peer_id}\n"], timeout=20):
            force_finish_mnsync(self.plain)
            self.wait_until(lambda: (self.mn_peer() or {}).get("verified_proregtx_hash") == protx, timeout=10)
        assert_equal(self.mn_peer()["id"], peer_id)
        # Held once, processed once: let the message loop pass over this peer many
        # more times, then read the whole window. A held MNAUTH that survived its
        # own processing would be replayed on every pass and punished as a duplicate
        # each time -- and the verified identity above would still look right.
        self.let_message_loop_run()
        window = self.log_since(log_start)
        assert_equal(window.count(f"{PROCESSED}, peer={peer_id}\n"), 1)
        assert "duplicate mnauth" not in window
        assert_equal(self.mn_peer()["id"], peer_id)
        self.disconnect_from_mn()

        self.log.info("3. gone before sync: the held MNAUTH leaves with its connection")
        self.reset_sync()
        with self.plain.assert_debug_log([HELD]):
            peer_id = self.connect_to_mn(["received: mnauth"])["id"]
        self.disconnect_from_mn()
        # Everything from here on is inside the window: sync finishing, the message
        # loop passing over every peer, and a fresh connection verifying -- by then
        # a held MNAUTH that outlived its connection would have been processed.
        with self.plain.assert_debug_log(["received: mnauth"], unexpected_msgs=[f"{PROCESSED}, peer={peer_id}\n"], timeout=20):
            force_finish_mnsync(self.plain)
            # and the next connection, after sync, is an ordinary one again
            self.connect_to_mn(["received: mnauth"])
            self.wait_until(lambda: self.mn_peer().get("verified_proregtx_hash") == protx, timeout=10)
        self.disconnect_from_mn()

        self.log.info("4. holding grants nothing: wrong size and unreadable are not held, an invalid signature is punished after sync")
        self.reset_sync()
        short = self.plain.add_p2p_connection(P2PInterface())
        short_id = self.plain.getpeerinfo()[-1]["id"]
        unreadable = self.plain.add_p2p_connection(P2PInterface())
        unreadable_id = self.plain.getpeerinfo()[-1]["id"]
        # NODE_BLOOM as well, or the handler would stop at the services check
        # before it ever reached the signature this case is about
        forged = self.plain.add_p2p_connection(P2PInterface(), services=P2P_SERVICES | NODE_BLOOM)
        forged_id = self.plain.getpeerinfo()[-1]["id"]
        with self.plain.assert_debug_log([f"{HELD}, peer={forged_id}\n", UNREADABLE, f"not held, peer={unreadable_id}\n"],
                                         unexpected_msgs=[f"{HELD}, peer={short_id}\n", f"{HELD}, peer={unreadable_id}\n"]):
            # one byte short of a proRegTxHash and a BLS signature
            short.send_and_ping(msg_generic(b"mnauth", b"\x01" * 127))
            # the right size, but the signature is the G2 identity (0xc0, then zeros): it
            # parses, is rejected as the identity, and writes back as zeros under both
            # schemes, so reading it throws -- which a held copy must never do later.
            # (Arbitrary bytes are not enough: some round-trip under one scheme.)
            unreadable.send_and_ping(msg_generic(b"mnauth", b"\x01" * 32 + b"\xc0" + b"\x00" * 95))
            # the right size, a made-up proRegTxHash and an all-zero signature: it reads,
            # as the null signature, and is never valid
            forged.send_and_ping(msg_generic(b"mnauth", b"\x01" * 32 + b"\x00" * 96))
        assert_equal(self.plain.mnsync("status")["IsBlockchainSynced"], False)
        with self.plain.assert_debug_log([f"{PROCESSED}, peer={forged_id}\n", "invalid mnauth signature"],
                                         unexpected_msgs=[f"{PROCESSED}, peer={short_id}\n",
                                                          f"{PROCESSED}, peer={unreadable_id}\n",
                                                          "(mnauth, held): Exception"], timeout=20):
            force_finish_mnsync(self.plain)
            forged.wait_for_disconnect(timeout=20)
            self.let_message_loop_run()
        # the node is still answering, and neither of the other two gained anything or was dropped
        short.sync_with_ping()
        unreadable.sync_with_ping()
        assert all("verified_proregtx_hash" not in p for p in self.plain.getpeerinfo() if p["id"] in (short_id, unreadable_id))


if __name__ == '__main__':
    MNAuthBeforeSyncTest().main()
