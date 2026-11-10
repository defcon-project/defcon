#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test logic for skipping signature validation on old blocks.

Test logic for skipping signature validation on blocks which we've assumed
valid (https://github.com/bitcoin/bitcoin/pull/9484)

We build a chain that includes and invalid signature for one of the
transactions:

    0:        genesis block
    1:        block 1 with coinbase transaction output.
    2 to COINBASE_MATURITY + 1:
              bury that block with COINBASE_MATURITY blocks so the coinbase
              transaction output can be spent
    COINBASE_MATURITY + 2:
              the bad block: a block containing a transaction spending the
              coinbase transaction output. The transaction has an invalid
              signature.
    then:     bury the bad block with just over two weeks' worth of blocks
              (8400 blocks, see run_test)

Start three nodes:

    - node0 has no -assumevalid parameter. Try to sync to the tip. It will
      reject the bad block and only sync as far as block COINBASE_MATURITY + 1
    - node1 has -assumevalid set to the hash of the bad block. node1 accepts
      the first 200 blocks, the bad block among them.
    - node2 has -assumevalid set to the hash of the bad block. Try to sync to
      block 200. node2 will reject the bad block since it's assumed valid, but
      it isn't buried by at least two weeks' work.
"""

from test_framework.blocktools import (
    COINBASE_MATURITY,
    create_block,
    create_coinbase,
)
from test_framework.key import ECKey
from test_framework.messages import (
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_block,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (CScript, OP_TRUE)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (assert_equal, set_node_times)


class BaseNode(P2PInterface):
    def send_header_for_blocks(self, new_blocks):
        headers_message = msg_headers()
        headers_message.headers = [CBlockHeader(b) for b in new_blocks]
        self.send_message(headers_message)


class AssumeValidTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = ["-dip3params=9000:9000", "-checkblockindex=0"]
        self.rpc_timeout = 120

    def setup_network(self):
        self.add_nodes(3)
        # Start node0. We don't start the other nodes yet since
        # we need to pre-mine a block with an invalid transaction
        # signature so we can pass in the block hash as assumevalid.
        self.start_node(0, extra_args=self.extra_args)

    def send_blocks_until_disconnected(self, p2p_conn):
        """Keep sending blocks to the node until we're disconnected."""
        for i in range(len(self.blocks)):
            if not p2p_conn.is_connected:
                break
            try:
                p2p_conn.send_message(msg_block(self.blocks[i]))
            except IOError:
                assert not p2p_conn.is_connected
                break

    def run_test(self):
        p2p0 = self.nodes[0].add_p2p_connection(BaseNode())

        # Build the blockchain
        self.tip = int(self.nodes[0].getbestblockhash(), 16)
        self.block_time = self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time'] + 1

        self.blocks = []

        # Get a pubkey for the coinbase TXO
        coinbase_key = ECKey()
        coinbase_key.generate()
        coinbase_pubkey = coinbase_key.get_pubkey().get_bytes()

        # Create the first block with a coinbase output to our key
        height = 1
        block = create_block(self.tip, create_coinbase(height, coinbase_pubkey), self.block_time)
        self.blocks.append(block)
        self.block_time += 1
        block.solve()
        # Save the coinbase for later
        self.block1 = block
        self.tip = block.sha256
        height += 1

        # Bury the block COINBASE_MATURITY deep so the coinbase output is spendable
        for _ in range(COINBASE_MATURITY):
            block = create_block(self.tip, create_coinbase(height), self.block_time)
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        # Create a transaction spending the coinbase output with an invalid (null) signature
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(self.block1.vtx[0].sha256, 0), scriptSig=b""))
        tx.vout.append(CTxOut(49 * 100000000, CScript([OP_TRUE])))
        tx.calc_sha256()

        bad_block = create_block(self.tip, create_coinbase(height), self.block_time)
        self.block_time += 1
        bad_block.vtx.extend([tx])
        bad_block.hashMerkleRoot = bad_block.calc_merkle_root()
        bad_block.solve()
        self.blocks.append(bad_block)
        self.tip = bad_block.sha256
        self.block_time += 1
        height += 1

        # Bury the assumed valid block 8400 deep (Dash needs 4x as much blocks to allow -assumevalid to work)
        for _ in range(8400):
            block = create_block(self.tip, create_coinbase(height), self.block_time)
            block.nVersion = 4
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        self.nodes[0].disconnect_p2ps()

        # Start node1 and node2 with assumevalid so they accept a block with a bad signature.
        self.start_node(1, extra_args=self.extra_args + ["-assumevalid=" + hex(bad_block.sha256)])
        self.start_node(2, extra_args=self.extra_args + ["-assumevalid=" + hex(bad_block.sha256)])

        p2p0 = self.nodes[0].add_p2p_connection(BaseNode())
        p2p1 = self.nodes[1].add_p2p_connection(BaseNode())
        p2p2 = self.nodes[2].add_p2p_connection(BaseNode())

        # Make sure nodes actually accept the many headers
        self.mocktime = self.block_time
        set_node_times(self.nodes, self.mocktime)

        # send header lists to all three nodes.
        # node0 does not need to receive all headers
        # node1 must receive all headers as otherwise assumevalid is ignored in ConnectBlock
        # node2 should NOT receive all headers to force skipping of the assumevalid check in ConnectBlock
        p2p0.send_header_for_blocks(self.blocks[0:2000])
        p2p1.send_header_for_blocks(self.blocks[0:2000])
        p2p1.send_header_for_blocks(self.blocks[2000:4000])
        p2p1.send_header_for_blocks(self.blocks[4000:6000])
        p2p1.send_header_for_blocks(self.blocks[6000:8000])
        p2p1.send_header_for_blocks(self.blocks[8000:])
        p2p2.send_header_for_blocks(self.blocks[0:200])

        # Send blocks to node0. The bad block will be rejected.
        self.send_blocks_until_disconnected(p2p0)
        self.wait_until(lambda: self.nodes[0].getblockcount() >= COINBASE_MATURITY + 1)
        assert_equal(self.nodes[0].getblockcount(), COINBASE_MATURITY + 1)

        # Send 200 blocks to node1. All blocks, including the bad block, will be accepted.
        for i in range(200):
            p2p1.send_message(msg_block(self.blocks[i]))
        # Syncing so many blocks can take a while on slow systems. Give it plenty of time to sync.
        p2p1.sync_with_ping(960)
        assert_equal(self.nodes[1].getblock(self.nodes[1].getbestblockhash())['height'], 200)

        # Send blocks to node2. The bad block will be rejected.
        self.send_blocks_until_disconnected(p2p2)
        self.wait_until(lambda: self.nodes[2].getblockcount() >= COINBASE_MATURITY + 1)
        assert_equal(self.nodes[2].getblockcount(), COINBASE_MATURITY + 1)


if __name__ == '__main__':
    AssumeValidTest().main()
