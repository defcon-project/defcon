#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test govsync Bloom filter size and hash-function limits.

Oversized vData must be rejected before allocation. An excessive nHashFuncs
must be rejected before request classification or a per-object vote walk,
where contains() would hash every vote that many times under a lock.
"""

import struct

from test_framework.messages import msg_generic, ser_compact_size, ser_string, ser_uint256
from test_framework.p2p import MESSAGEMAP, P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

# CBloomFilter limits (src/common/bloom.h).
MAX_BLOOM_FILTER_SIZE = 36000
MAX_HASH_FUNCS = 50
MASTERNODE_SYNC_GOVOBJ = 10

# serialize.h MAX_SIZE: the largest count ReadCompactSize() accepts, so a declared
# vData length of this value reaches the vData cap, not the compact-size guard.
MAX_SIZE = 0x02000000


def govsync(nprop=0, data=b"", n_hash_funcs=0):
    payload = ser_uint256(nprop) + ser_string(data)
    payload += struct.pack("<IIB", n_hash_funcs, 0, 0)  # hash functions, tweak, flags
    return msg_generic(b"govsync", payload)


class msg_ssc:
    """Decode the sync-status response to an accepted govsync request."""
    msgtype = b"ssc"

    def deserialize(self, stream):
        self.item_id, self.count = struct.unpack("<ii", stream.read(8))

    def __repr__(self):
        return f"msg_ssc(item_id={self.item_id}, count={self.count})"


class GovsyncPeer(P2PInterface):
    def on_ssc(self, message):
        pass


class GovsyncBloomCapTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def assert_rejected(self, message):
        node = self.nodes[0]
        peer = node.add_p2p_connection(GovsyncPeer())
        peer_id = node.getpeerinfo()[0]["id"]
        with node.assert_debug_log(
            [f"Misbehaving: peer={peer_id} (0 -> 100)"],
            unexpected_msgs=[
                "MNGOVERNANCESYNC -- syncing governance objects",
                "CGovernanceManager::SyncSingleObjVotes",
            ],
        ):
            peer.send_message(message)
            peer.wait_for_disconnect(timeout=5)
        node.disconnect_p2ps()

    def run_test(self):
        node = self.nodes[0]
        # The shared framework does not decode this Dash-specific response.
        # Register it locally so accepted requests are actually checked.
        MESSAGEMAP[b"ssc"] = msg_ssc
        # The handler ignores govsync until masternode sync completes.
        force_finish_mnsync(node)

        self.log.info("Empty and maximum-size filters at the hash-function boundary allow full sync")
        for data, n_hash_funcs in ((b"", 0), (b"\xff" * MAX_BLOOM_FILTER_SIZE, MAX_HASH_FUNCS)):
            peer = node.add_p2p_connection(GovsyncPeer())
            with node.assert_debug_log([], unexpected_msgs=["Misbehaving"]):
                peer.send_message(govsync(data=data, n_hash_funcs=n_hash_funcs))
                peer.sync_with_ping()
                with p2p_lock:
                    assert_equal(peer.message_count["ssc"], 1)
                    assert_equal(peer.last_message["ssc"].item_id, MASTERNODE_SYNC_GOVOBJ)
                    assert_equal(peer.last_message["ssc"].count, 0)
            assert peer.is_connected
            node.disconnect_p2ps()

        self.log.info("A nonempty filter at 50 hash functions reaches the per-object sync handler")
        peer = node.add_p2p_connection(GovsyncPeer())
        with node.assert_debug_log(
            ["CGovernanceManager::SyncSingleObjVotes -- no matching object"],
            unexpected_msgs=["Misbehaving"],
        ):
            peer.send_message(govsync(nprop=1, data=b"\xff", n_hash_funcs=MAX_HASH_FUNCS))
            peer.sync_with_ping()
        assert peer.is_connected
        node.disconnect_p2ps()

        self.log.info("Above-limit hash counts score 100 before full-sync, object-fetch, or vote-sync handling")
        # No matching object/votes exist, so running this regression on an
        # unfixed node cannot trigger the expensive hash loop, even at UINT32_MAX.
        for nprop in (0, 1):
            for data in (b"\xff", b""):
                for n_hash_funcs in (MAX_HASH_FUNCS + 1, 0xFFFFFFFF):
                    self.log.info(f"Reject nprop={nprop}, filter bytes={len(data)}, nHashFuncs={n_hash_funcs}")
                    self.assert_rejected(govsync(nprop, data, n_hash_funcs))

        self.log.info("A filter one byte above the data limit is rejected")
        self.assert_rejected(govsync(data=b"\xff" * (MAX_BLOOM_FILTER_SIZE + 1), n_hash_funcs=MAX_HASH_FUNCS))

        self.log.info("A govsync request declaring an oversized filter vData length with the bytes omitted is rejected before allocation")
        # nProp (32 bytes) then a CompactSize(MAX_SIZE) vData length with no bytes. Without the
        # cap this would fall into net_processing's outer catch (no Misbehaving, no disconnect).
        raw_payload = ser_uint256(0) + ser_compact_size(MAX_SIZE)
        self.assert_rejected(msg_generic(b'govsync', raw_payload))


if __name__ == '__main__':
    GovsyncBloomCapTest().main()
