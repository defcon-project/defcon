#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Quorum rotation (DIP0024) is gone.

1. getqrinfo and qrinfo are ignored: no answer, no penalty, and the connection stays, over the
   plain transport and over v2, where both keep their short IDs (166 and 167).
2. The quorum rotationinfo RPC is gone.
3. Regtest has no dip0024 softfork and no rotating quorum type; llmq_test_instantsend, which
   signs InstantSend here, is enabled; -testactivationheight=dip0024@<height> is refused.
"""

import struct

from test_framework.messages import ser_compact_size, ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.v2_p2p import MSGTYPE_TO_SHORTID


class msg_raw:
    """A message the framework has no class for, sent as raw bytes."""

    def __init__(self, msgtype, payload=b""):
        self.msgtype = msgtype
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return f"msg_raw({self.msgtype!r}, {len(self.payload)} bytes)"


def getqrinfo(request_hash):
    """No base block hashes, the request block, no extra share."""
    return msg_raw(b"getqrinfo", ser_compact_size(0) + ser_uint256(int(request_hash, 16)) + struct.pack("<?", False))


class LLMQNoRotationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-v2transport=1"]]

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 10)
        tip = node.getbestblockhash()

        self.log.info("The two message names keep their v2 short IDs")
        assert_equal(MSGTYPE_TO_SHORTID[b"getqrinfo"], 166)
        assert_equal(MSGTYPE_TO_SHORTID[b"qrinfo"], 167)

        for v2 in (False, True):
            self.log.info(f"getqrinfo and qrinfo are ignored without penalty ({'v2' if v2 else 'plain'} transport)")
            peer = node.add_p2p_connection(P2PInterface(), supports_v2_p2p=v2)
            assert_equal(peer.supports_v2_p2p, v2)
            with node.assert_debug_log(expected_msgs=["ignoring getqrinfo from peer=", "ignoring qrinfo from peer="],
                                       unexpected_msgs=["Misbehaving"]):
                for _ in range(3):
                    peer.send_message(getqrinfo(tip))
                peer.send_message(msg_raw(b"qrinfo"))
                peer.sync_with_ping()
            assert "qrinfo" not in peer.message_count
            assert peer.is_connected
            assert_equal(len(node.getpeerinfo()), 1)
            node.disconnect_p2ps()

        self.log.info("quorum rotationinfo is gone")
        assert_raises_rpc_error(-1, 'quorum "command"', node.quorum, "rotationinfo", tip)
        assert "rotationinfo" not in node.help("quorum")

        self.log.info("Regtest has no dip0024 softfork and no rotating quorum type")
        assert "dip0024" not in node.getblockchaininfo()["softforks"]
        types = node.quorum("list")
        assert "llmq_test_dip0024" not in types
        assert "llmq_test_instantsend" in types

        self.log.info("-testactivationheight=dip0024@<height> is refused")
        self.stop_node(0)
        node.assert_start_raises_init_error(
            extra_args=["-testactivationheight=dip0024@100"],
            expected_msg=r"Invalid name \(dip0024@100\) for -testactivationheight=name@height\.",
            match=ErrorMatch.PARTIAL_REGEX)


if __name__ == "__main__":
    LLMQNoRotationTest().main()
