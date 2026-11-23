#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""From the v23.1 height a block carries no commitment of a quorum type that is not enabled for it.

llmq_test_v17 stops forming at END, as llmq_400_60 and llmq_400_85 do at the v23.1
height on the release networks. Here the v23.1 height comes forty blocks later, in
the middle of a cycle, so one chain shows both sides of it. There is one
masternode, so no quorum forms and every commitment on the chain is a null one.
The null commitment of the retired type is made from one the miner put into a
block while the type was enabled: its height and the base block of its cycle are
changed, nothing else.

1. Below END the miner puts a null llmq_test_v17 commitment into a block of the
   mining window, and none from END on.
2. Between END and the v23.1 height a block that carries one is taken, up to the
   last block below the height: the checks count the enabled types only, and a
   null commitment passes on its shape. Every block below the height is judged
   this way, by this version as by the ones before it.
3. From the v23.1 height such a block is refused (bad-qc-not-allowed), at the
   height and after it, while llmq_test goes on mining its commitments.
4. The other node has the same chain, and -reindex arrives at the same tip.
"""

import struct
from io import BytesIO

from test_framework.messages import CFinalCommitment, CTransaction, from_hex
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

INTERVAL = 24          # dkgInterval of both profiles
WINDOW = (10, 18)      # their dkgMiningWindowStart/End
END = 720              # on llmq_test_v17's grid
V231 = END + 40        # inside the cycle based at END + 24, and below the 900 at which regtest turns v20 on
V17_TYPE = 102
TEST_TYPE = 100
QUORUM_COMMITMENT_TX = 6
NEVER = 2**31 - 1
REFUSED = "TestBlockValidity failed: bad-qc-not-allowed"


class V231RetiredCommitmentTest(DashTestFramework):
    def set_test_params(self):
        args = ["-vbparams=testdummy:0:999999999999:0:10:8:6:5:0",
                "-llmqformationendheight=llmq_test_v17:%d" % END,
                "-testactivationheight=v231@%d" % V231,
                "-testactivationheight=superblocksretired@%d" % V231,
                "-testactivationheight=mn_rr@%d" % NEVER]
        self.set_dash_test_params(2, 1, extra_args=[args] * 2)

    def commitments(self, height):
        """The quorum commitment transactions of the block at this height, as getblock lists them."""
        node = self.nodes[0]
        block = node.getblock(node.getblockhash(height), 2)
        return [tx for tx in block["tx"] if tx.get("type") == QUORUM_COMMITMENT_TX and "qcTx" in tx]

    def commitment_types(self, height):
        return [tx["qcTx"]["commitment"]["llmqType"] for tx in self.commitments(height)]

    def listed(self):
        return "llmq_test_v17" in self.nodes[0].quorum("list")

    def generate_to(self, height):
        tip = self.nodes[0].getblockcount()
        assert height >= tip, (height, tip)
        while tip < height:
            n = min(10, height - tip)
            self.bump_mocktime(n)
            self.generate(self.nodes[0], n)
            tip += n
        assert_equal(self.nodes[0].getblockcount(), height)

    def retired_commitment(self, height):
        """The template for a block at `height`: that height, and the base block of the cycle the block is in."""
        assert height % INTERVAL != 0   # a cycle's base block cannot name itself
        tx = from_hex(CTransaction(), self.template)
        payload = BytesIO(tx.vExtraPayload)
        version = payload.read(2)
        payload.read(4)
        commitment = CFinalCommitment()
        commitment.deserialize(payload)
        assert_equal(payload.read(), b"")
        commitment.quorumHash = int(self.nodes[0].getblockhash(height - height % INTERVAL), 16)
        tx.vExtraPayload = version + struct.pack("<I", height) + commitment.serialize()
        tx.rehash()
        return tx.serialize().hex()

    def block_with(self, height):
        """Mine the block at `height` with the retired type's null commitment in it."""
        node = self.nodes[0]
        assert_equal(node.getblockcount(), height - 1)
        self.bump_mocktime(1)
        block_hash = self.generateblock(node, self.address, [self.retired_commitment(height)])["hash"]
        assert_equal(node.getbestblockhash(), block_hash)
        assert_equal(node.getblockcount(), height)
        assert V17_TYPE in self.commitment_types(height)

    def refused_with(self, height):
        """The same block at `height` is not made: the node refuses it before any work is done on it."""
        node = self.nodes[0]
        assert_equal(node.getblockcount(), height - 1)
        self.bump_mocktime(1)
        assert_raises_rpc_error(-25, REFUSED, node.generateblock, self.address, [self.retired_commitment(height)],
                                invalid_call=False)
        assert_equal(node.getblockcount(), height - 1)

    def run_test(self):
        node = self.nodes[0]
        self.address = node.getnewaddress()
        self.wait_for_sporks_same()
        start = node.getblockcount()
        self.log.info("height after setup %d, END %d, the v23.1 height %d", start, END, V231)
        last_below = END - INTERVAL
        assert_greater_than(last_below, start)

        self.log.info("1. Below END the miner adds a null llmq_test_v17 commitment in the mining window")
        in_window = last_below + WINDOW[0]
        self.generate_to(in_window)
        assert self.listed()
        mined = [tx for tx in self.commitments(in_window) if tx["qcTx"]["commitment"]["llmqType"] == V17_TYPE]
        assert_equal(len(mined), 1)
        assert_equal(mined[0]["qcTx"]["commitment"]["signersCount"], 0)
        self.template = node.getrawtransaction(mined[0]["txid"], False, node.getblockhash(in_window))
        # the classes here read and write the payload as the node does
        tx = from_hex(CTransaction(), self.template)
        commitment = CFinalCommitment()
        commitment.deserialize(BytesIO(tx.vExtraPayload[6:]))
        assert_equal(tx.vExtraPayload[:6] + commitment.serialize(), tx.vExtraPayload)
        assert_equal(commitment.llmqType, V17_TYPE)
        assert_equal(struct.unpack("<I", tx.vExtraPayload[2:6])[0], in_window)

        self.generate_to(END + 2)
        assert not self.listed()
        for height in range(END, END + 3):
            assert V17_TYPE not in self.commitment_types(height), height

        self.log.info("2. Between END and the v23.1 height a block with the retired type's null commitment is taken")
        self.block_with(END + 3)
        assert not self.listed()
        self.generate_to(V231 - 2)
        self.block_with(V231 - 1)

        self.log.info("3. From the v23.1 height it is refused, at the height and after it")
        self.refused_with(V231)
        self.generate_to(V231 + 2)
        self.refused_with(V231 + 3)

        self.log.info("llmq_test, which is enabled, goes on: its null commitments are mined in the next window")
        next_base = V231 - V231 % INTERVAL + INTERVAL
        self.generate_to(next_base + WINDOW[1])
        window = [t for height in range(next_base + WINDOW[0], next_base + WINDOW[1] + 1) for t in self.commitment_types(height)]
        assert TEST_TYPE in window, window
        assert V17_TYPE not in window, window

        self.log.info("4. The other node has the same chain, and -reindex arrives at the same tip")
        self.sync_blocks()
        tip = node.getbestblockhash()
        assert_equal(self.nodes[1].getbestblockhash(), tip)
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: node.getbestblockhash() == tip, timeout=240)


if __name__ == '__main__':
    V231RetiredCommitmentTest().main()
