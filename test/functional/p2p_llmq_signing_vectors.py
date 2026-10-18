#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Malformed within-cap LLMQ signing and DKG vectors must score and disconnect."""

from test_framework.messages import CSigShare, msg_generic, ser_compact_size, ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

MAX_SIZE = 0x02000000  # largest CompactSize the node accepts
MAX_LLMQ_SIZE = 400


def ser_varint(n):
    """Serialize an unsigned integer with the C++ VARINT (VarIntMode::DEFAULT) encoding."""
    r = bytearray()
    while True:
        r.append((n & 0x7f) | (0x80 if r else 0x00))
        if n <= 0x7f:
            break
        n = (n >> 7) - 1
    return bytes(reversed(r))


def ser_sigshares_inv(inv_size, offsets):
    """One CSigSharesInv in the varint AUTOBITSET encoding with the given bit offsets."""
    r = ser_varint(0)  # session id
    r += ser_compact_size(inv_size)
    r += b"\x01"  # AUTOBITSET: select the VarInts bitset encoding
    for offset in offsets:
        r += ser_varint(offset)
    r += ser_varint(0)  # stopper
    return r


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

        # A CSigSharesInv carries one bit per quorum member. Its bitset is bounded
        # before it is allocated and a varint offset cannot wrap the index. Each
        # case is one well-formed vector element with a single bad field, and the
        # exact rejection reason must show up, not just any failure.
        bad_invs = {
            "wrapping first offset": (ser_sigshares_inv(MAX_LLMQ_SIZE, [0x80000001]), "out of bounds index"),
            "wrapping later offset": (ser_sigshares_inv(MAX_LLMQ_SIZE, [10, 0xffffffff]), "out of bounds index"),
            "offset past the size": (ser_sigshares_inv(MAX_LLMQ_SIZE, [MAX_LLMQ_SIZE + 1]), "out of bounds index"),
            "declared size above the largest quorum": (ser_sigshares_inv(MAX_LLMQ_SIZE + 1, []), "CSigSharesInv::inv size too large"),
            "declared size at the CompactSize maximum": (ser_sigshares_inv(MAX_SIZE, []), "CSigSharesInv::inv size too large"),
        }
        for msg_type in (b"qsigsinv", b"qgetsigs"):
            label = msg_type.decode()
            for case, (inv, reason) in bad_invs.items():
                self.log.info(f"{label} with {case} scores 100 and disconnects")
                peer = node.add_p2p_connection(P2PInterface(), uacomment=f"inv-{label}")
                peer_id = self.peer_id(node, peer)
                try:
                    with node.assert_debug_log([
                        f"rejected {label} from peer={peer_id}: {reason}",
                        f"Misbehaving: peer={peer_id} (0 -> 100)",
                    ]):
                        peer.send_message(msg_generic(msg_type, ser_compact_size(1) + inv))
                        peer.wait_for_disconnect(timeout=5)
                except AssertionError as error:
                    failures.append((f"{label}: {case}", str(error)))
                node.disconnect_p2ps()

        # A QSIGSHARE carries its LLMQ type as an unvalidated byte. A type this
        # chain does not register must be rejected before the quorum lookup: the
        # quorum caches only hold the registered types.
        tip_hash = int(node.getbestblockhash(), 16)
        for llmq_type in (0, 6):  # not an enumerator; LLMQ_25_67, registered on no DeFCoN chain
            self.log.info(f"qsigshare with unregistered llmqType {llmq_type} scores 100 and disconnects")
            sig_share = CSigShare()
            sig_share.llmqType = llmq_type
            sig_share.quorumHash = tip_hash
            peer = node.add_p2p_connection(P2PInterface(), uacomment=f"sigshare-type-{llmq_type}")
            peer_id = self.peer_id(node, peer)
            try:
                with node.assert_debug_log([
                    f"invalid llmqType[{llmq_type}] from peer={peer_id}",
                    f"Misbehaving: peer={peer_id} (0 -> 100)",
                ]):
                    peer.send_message(msg_generic(b"qsigshare", ser_compact_size(1) + sig_share.serialize()))
                    peer.wait_for_disconnect(timeout=5)
            except AssertionError as error:
                failures.append((f"qsigshare: llmqType {llmq_type}", str(error)))
            node.disconnect_p2ps()

        # Genesis has no parent, so it can never be a quorum base. A DKG message
        # naming it must be rejected before the quorum activation check.
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        genesis_hash = node.getblockhash(0)
        llmq_type_test = 100  # LLMQ_TEST, registered on regtest
        self.log.info("qcontrib naming the genesis block as its quorum base scores 100 and disconnects")
        peer = node.add_p2p_connection(P2PInterface(), uacomment="dkg-genesis")
        peer_id = self.peer_id(node, peer)
        try:
            with node.assert_debug_log([
                f"parentless quorumHash {genesis_hash}",
                f"Misbehaving: peer={peer_id} (0 -> 100)",
            ]):
                peer.send_message(msg_generic(b"qcontrib", bytes([llmq_type_test]) + ser_uint256(int(genesis_hash, 16))))
                peer.wait_for_disconnect(timeout=5)
        except AssertionError as error:
            failures.append(("qcontrib: genesis quorum base", str(error)))
        node.disconnect_p2ps()

        # An inventory for the largest quorum still decodes, and the node is
        # still serving requests after all of the above.
        peer = node.add_p2p_connection(P2PInterface(), uacomment="inv-valid")
        peer.send_message(msg_generic(b"qsigsinv", ser_compact_size(1) + ser_sigshares_inv(MAX_LLMQ_SIZE, [1, 399])))
        peer.sync_with_ping()
        assert peer.is_connected
        node.disconnect_p2ps()
        assert_equal(node.getblockcount(), self.nodes[0].getblockcount())
        assert_equal(failures, [])


if __name__ == '__main__':
    LLMQSigningVectorTest().main()
