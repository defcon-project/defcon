#!/usr/bin/env python3
# Copyright (c) 2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test the request expiration of governance object and vote announcements.

The net layer keeps each peer's announcements and in-flight requests (#313 replaced
the governance manager's m_requested_hash_time cache with it). For one peer and one
hash, counted in GETDATA messages:

  1  the first announcement is requested;
  2  announced again after GETDATA_TX_INTERVAL, with the request still in flight,
     it is not requested again;
  3  once the in-flight request has expired (GetObjectExpiryInterval) and the peer's
     expiry sweep has run, announced again, it is requested again.
"""

from collections import defaultdict

from test_framework.messages import (
    CInv,
    msg_inv,
    MSG_GOVERNANCE_OBJECT,
    MSG_GOVERNANCE_OBJECT_VOTE,
)
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

GETDATA_TX_INTERVAL = 60                               # src/net_processing.cpp
OBJECT_EXPIRY_INTERVAL = 10 * GETDATA_TX_INTERVAL      # GetObjectExpiryInterval
EXPIRY_SWEEP_MAX_GAP = OBJECT_EXPIRY_INTERVAL * 3 // 2  # a peer's expiry sweeps are at most this far apart


class GetDataCounter(P2PInterface):
    def __init__(self):
        super().__init__()
        self.getdata_count = defaultdict(int)

    def on_getdata(self, message):
        for inv in message.inv:
            self.getdata_count[(inv.type, inv.hash)] += 1


class GovernanceInvsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.test_request_expiration(MSG_GOVERNANCE_OBJECT, 1, "object")
        self.test_request_expiration(MSG_GOVERNANCE_OBJECT_VOTE, 2, "vote")

    def test_request_expiration(self, inv_type, inv_hash, name):
        node = self.nodes[0]
        peer = node.add_p2p_connection(GetDataCounter())
        # Going from no connection to one resets an outdated sync (CConnman::NotifyNumConnectionsChanged), and the
        # clock moved on by the expiry here: finish the sync after the peer is connected, or nothing is requested.
        peer.sync_with_ping()
        force_finish_mnsync(node)
        key = (inv_type, inv_hash)
        inv = msg_inv([CInv(inv_type, inv_hash)])

        def requested():
            # A GETDATA for an announcement goes out before the pong of a ping sent after it.
            peer.sync_with_ping()
            with p2p_lock:
                return peer.getdata_count[key]

        self.log.info(f"Announce a dummy governance {name}: it is requested")
        peer.send_message(inv)
        peer.wait_until(lambda: peer.getdata_count[key] == 1)

        self.log.info(f"Announce it again after {GETDATA_TX_INTERVAL + 1} s, the request still in flight: not requested again")
        self.bump_mocktime(GETDATA_TX_INTERVAL + 1, nodes=[node])
        peer.send_message(inv)
        assert_equal(requested(), 1)

        self.log.info("Let the request expire and the peer's expiry sweep run, announce it again: requested again")
        self.bump_mocktime(EXPIRY_SWEEP_MAX_GAP + 1, nodes=[node])
        peer.sync_with_ping()
        # The sweep alone requests nothing: the second GETDATA has to come from the next announcement.
        assert_equal(requested(), 1)
        peer.send_message(inv)
        peer.wait_until(lambda: peer.getdata_count[key] == 2)
        assert_equal(requested(), 2)
        node.disconnect_p2ps()


if __name__ == '__main__':
    GovernanceInvsTest().main()
