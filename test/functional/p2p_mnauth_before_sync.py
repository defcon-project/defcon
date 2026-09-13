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
   connection, no reconnect.
3. A connection that goes away before sync takes its held MNAUTH with it.
4. Holding buys time, not trust: a message of the wrong size is not held, and a
   well-sized one with an invalid signature is judged after sync by the ordinary
   handler, with its ordinary penalty.
"""

from test_framework.messages import NODE_BLOOM, msg_generic
from test_framework.p2p import P2P_SERVICES, P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync, p2p_port

HELD = "MNAUTH received before blockchain sync, held until it finishes"
PROCESSED = "processing the MNAUTH held until blockchain sync"


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
        # the trailing newline keeps peer=3 from matching peer=31
        with self.plain.assert_debug_log([f"{PROCESSED}, peer={peer_id}\n"], timeout=20):
            force_finish_mnsync(self.plain)
            self.wait_until(lambda: (self.mn_peer() or {}).get("verified_proregtx_hash") == protx, timeout=10)
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

        self.log.info("4. holding grants nothing: wrong size is not held, an invalid signature is punished after sync")
        self.reset_sync()
        short = self.plain.add_p2p_connection(P2PInterface())
        short_id = self.plain.getpeerinfo()[-1]["id"]
        # NODE_BLOOM as well, or the handler would stop at the services check
        # before it ever reached the signature this case is about
        forged = self.plain.add_p2p_connection(P2PInterface(), services=P2P_SERVICES | NODE_BLOOM)
        forged_id = self.plain.getpeerinfo()[-1]["id"]
        with self.plain.assert_debug_log([f"{HELD}, peer={forged_id}\n"],
                                         unexpected_msgs=[f"{HELD}, peer={short_id}\n"]):
            # one byte short of a proRegTxHash and a BLS signature
            short.send_and_ping(msg_generic(b"mnauth", b"\x01" * 127))
            # the right size, a made-up proRegTxHash and an all-zero signature, which is never valid
            forged.send_and_ping(msg_generic(b"mnauth", b"\x01" * 32 + b"\x00" * 96))
        assert_equal(self.plain.mnsync("status")["IsBlockchainSynced"], False)
        with self.plain.assert_debug_log([f"{PROCESSED}, peer={forged_id}\n", "invalid mnauth signature"],
                                         unexpected_msgs=[f"{PROCESSED}, peer={short_id}\n"], timeout=20):
            force_finish_mnsync(self.plain)
            forged.wait_for_disconnect(timeout=20)
            short.sync_with_ping()
        assert all("verified_proregtx_hash" not in p for p in self.plain.getpeerinfo() if p["id"] == short_id)


if __name__ == '__main__':
    MNAuthBeforeSyncTest().main()
