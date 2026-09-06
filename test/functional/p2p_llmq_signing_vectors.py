#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Malformed within-cap LLMQ signing vectors must score and disconnect."""

from test_framework.messages import msg_generic, ser_compact_size
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal


class LLMQSigningVectorTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(2, 1)

    @staticmethod
    def peer_id(node, peer):
        return next(info["id"] for info in node.getpeerinfo() if info["subver"] == peer.strSubVer)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_21_QUORUM_ALL_CONNECTED", 0)
        self.wait_for_sporks_same()
        node = self.mninfo[0].node

        # Count 1 is below every affected cap, but no element bytes follow.
        # The element read throws after the allocation bound accepted the count.
        payload = ser_compact_size(1)
        failures = []
        for msg_type in (b"qsigshare", b"qsigsesann", b"qsigsinv", b"qgetsigs"):
            label = msg_type.decode()
            self.log.info(f"Malformed within-cap {label} scores 100 and disconnects")
            peer = node.add_p2p_connection(P2PInterface(), uacomment=f"f016-{label}")
            peer_id = self.peer_id(node, peer)
            try:
                with node.assert_debug_log([
                    f"rejected {label} from peer={peer_id}",
                    f"Misbehaving: peer={peer_id} (0 -> 100)",
                ]):
                    peer.send_message(msg_generic(msg_type, payload))
                    peer.wait_for_disconnect(timeout=5)
            except AssertionError as error:
                # Collect every malformed message result, so the unpatched
                # negative control proves all four missing score paths.
                failures.append((label, str(error)))
            node.disconnect_p2ps()
        assert_equal(failures, [])


if __name__ == '__main__':
    LLMQSigningVectorTest().main()
