#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a govsync bloom filter with an oversized vData declaration is
rejected before allocation.

CBloomFilter deserialization is bounded (LIMITED_VECTOR on vData), and the
MNGOVERNANCESYNC handler attributes the failure to the peer instead of letting
the outer catch drop it silently, so the peer is punished and disconnected.
"""

from test_framework.messages import msg_generic, ser_compact_size, ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import force_finish_mnsync

# serialize.h MAX_SIZE: the largest count ReadCompactSize() accepts, so a declared
# vData length of this value reaches the vData cap, not the compact-size guard.
MAX_SIZE = 0x02000000


class GovsyncBloomCapTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        # The handler ignores govsync until masternode sync completes.
        force_finish_mnsync(node)

        self.log.info("A govsync request declaring an oversized filter vData length with the bytes omitted is rejected before allocation")
        # nProp (32 bytes) then a CompactSize(MAX_SIZE) vData length with no bytes. Without the
        # cap this would fall into net_processing's outer catch (no Misbehaving, no disconnect).
        raw_peer = node.add_p2p_connection(P2PInterface())
        raw_payload = ser_uint256(0) + ser_compact_size(MAX_SIZE)
        with node.assert_debug_log(['Misbehaving']):
            raw_peer.send_message(msg_generic(b'govsync', raw_payload))
            raw_peer.wait_for_disconnect()


if __name__ == '__main__':
    GovsyncBloomCapTest().main()
