#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Header announcements on the uncompressed path.

A peer that does not advertise NODE_HEADERS_COMPRESSED is served the plain
`headers` message (UsesCompressedHeaders, net_processing.cpp), and the node
writes that message as a vector of CBlocks carrying no transactions: 80 bytes
of header followed by a 0x00 transaction count, 81 bytes per header. The
reader used to consume one byte more than that -- a length prefix for
vchBlockSig, which the block serializer only ever writes for a block whose
vtx[1] is a coinstake, and a header message has no transactions at all.

The consequence was that the node could not read the very message it produces:
every announcement on this path died in a caught deserialization exception,
with no misbehaviour score, no disconnect and no header reaching the block
index. This test announces a header the ordinary way and insists that it
arrives, then hands the node its own headers message back and insists that it
parses.
"""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import (
    CBlockHeader,
    NODE_NETWORK,
    NODE_HEADERS_COMPRESSED,
    msg_getheaders,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

# 80-byte header plus the 0x00 transaction count of an empty CBlock
BYTES_PER_HEADER = 81


class UncompressedHeadersPeer(P2PInterface):
    """A peer that speaks the classic header format, as every implementation
    without the compressed-headers service bit does."""

    def __init__(self):
        super().__init__()
        self.headers_messages = []

    def on_headers(self, message):
        self.headers_messages.append(message)


class P2PHeadersUncompressedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(UncompressedHeadersPeer(), services=NODE_NETWORK)
        # the service bit is what selects the path under test
        assert_equal(NODE_NETWORK & NODE_HEADERS_COMPRESSED, 0)

        self.log.info("An announced header reaches the block index")
        tip_hash = node.getbestblockhash()
        tip = node.getblock(tip_hash)
        block = create_block(int(tip_hash, 16), create_coinbase(tip["height"] + 1), tip["time"] + 1)
        block.solve()

        peer.send_message(msg_headers([CBlockHeader(block)]))
        peer.sync_with_ping()
        header = node.getblockheader(block.hash)
        assert_equal(header["height"], tip["height"] + 1)
        assert_equal(header["previousblockhash"], tip_hash)

        self.log.info("The node can read the headers message it writes itself")
        getheaders = msg_getheaders()
        getheaders.locator.vHave = [int(node.getblockhash(0), 16)]
        getheaders.hashstop = 0
        peer.send_message(getheaders)
        peer.wait_until(lambda: len(peer.headers_messages) > 0)

        answer = peer.headers_messages[0]
        assert_greater_than(len(answer.headers), 1)
        # one compact-size count for the vector, then one entry per header
        assert_equal(len(answer.serialize()), 1 + BYTES_PER_HEADER * len(answer.headers))

        echo = msg_headers([CBlockHeader(h) for h in answer.headers])
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["ProcessMessages(headers"]):
            peer.send_message(echo)
            peer.sync_with_ping()

        # the peer is still a peer: a parse failure here used to be silent, so
        # the absence of a disconnect is not on its own evidence of success
        assert_equal(len(node.getpeerinfo()), 1)


if __name__ == "__main__":
    P2PHeadersUncompressedTest().main()
