#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The masternode-list calls on a block that has never been connected.

A block's masternode list exists once the block has been connected. For a
stored block that never was -- here two blocks of a branch with as much work as
the tip, which the node keeps but does not switch to -- protx info, protx
listdiff and masternode payments say so (RPC_INVALID_PARAMETER), instead of
failing on the missing list; protx diff, which takes blocks of the active chain
only, keeps its own answer. masternode payments reads the parent's list, so
a block whose parent was connected still gets its answer, as do connected
blocks, the blocks before DIP0003 (genesis among them, whose list is empty by
rule), and blocks that were connected and then invalidated, whose lists stay.

A stored block that was never connected has never had its inputs checked
either -- here two blocks on the tip whose connection fails, one spending an
output that does not exist, one paying out more than it spends. masternode
payments adds up a block's fees from its inputs, so it checks them first, as
connecting the block does, and says the block is not valid (RPC_MISC_ERROR)
instead of reading past the end of a transaction's outputs.

The plain framework keeps regtest's own DIP0003Height (432), so the lists are
real from there on.
"""

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_block
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

CHAIN_HEIGHT = 450  # past DIP0003 (432)
SOME_PROTX = "00" * 32
NO_SUCH_OUTPUT = 0xfffffffe  # far past the end of any transaction's outputs, and not the null outpoint's index


class MasternodeListsUnconnectedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def spend(self, txid, n, value):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(value, CScript([OP_TRUE]))]
        tx.rehash()
        return tx

    def stored_on_tip(self, node, tx):
        """A block on the tip with `tx` in it: the node stores it, fails to connect it and keeps its tip."""
        tip = node.getbestblockhash()
        block = create_block(tmpl=node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS), txlist=[tx])
        block.solve()
        assert node.submitblock(block.serialize().hex()) is not None  # the reason it was not connected
        assert_equal(node.getbestblockhash(), tip)
        node.getblock(block.hash, 0)  # fails unless the block is stored
        return block.hash

    def run_test(self):
        node, other = self.nodes

        self.log.info("masternode payments checks the inputs of a block never connected before adding them up")
        # Below DIP0003 (432) a plain coinbase does, and the list the payments are read from is empty by rule.
        assert node.getblockcount() < 432
        first_coinbase = node.getblock(node.getblockhash(1))["tx"][0]  # found through -txindex
        no_such_output = self.spend(first_coinbase, NO_SUCH_OUTPUT, COIN)
        block_hash = self.stored_on_tip(node, no_such_output)
        assert_raises_rpc_error(
            -1, "Block %s is not valid: transaction %s spends output %d of transaction %s, which does not exist" % (
                block_hash, no_such_output.hash, NO_SUCH_OUTPUT, first_coinbase),
            node.masternode, "payments", block_hash)
        second_coinbase = node.getblock(node.getblockhash(2), 2)["tx"][0]
        paid = int(second_coinbase["vout"][0]["value"] * COIN)
        pays_more = self.spend(second_coinbase["txid"], 0, paid + COIN)
        block_hash = self.stored_on_tip(node, pays_more)
        assert_raises_rpc_error(
            -1, "Block %s is not valid: transaction %s pays out more than its inputs" % (block_hash, pays_more.hash),
            node.masternode, "payments", block_hash)
        # the node is still up, and its tip is answered as before
        assert_equal(node.masternode("payments")[0]["blockhash"], node.getbestblockhash())

        self.generate(node, CHAIN_HEIGHT - node.getblockcount())
        fork_point = node.getblockcount()

        self.log.info("Two branches of two blocks each; the node keeps its own and stores the other one")
        self.disconnect_nodes(0, 1)
        own = self.generate(node, 2, sync_fun=self.no_op)
        branch = self.generate(other, 2, sync_fun=self.no_op)
        for block_hash in branch:
            node.submitblock(other.getblock(block_hash, 0))
        assert_equal(node.getbestblockhash(), own[-1])
        tips = {tip["hash"]: tip["status"] for tip in node.getchaintips()}
        assert_equal(tips[branch[-1]], "valid-headers")
        first, second = branch

        self.log.info("protx info and protx listdiff refuse a block never connected, saying why")
        for block_hash in (first, second):
            message = "Block %s has not been connected, so it has no masternode list" % block_hash
            assert_raises_rpc_error(-8, message, node.protx, "info", SOME_PROTX, block_hash)
            assert_raises_rpc_error(-8, message, node.protx, "listdiff", fork_point, block_hash)
            # protx diff answers only for blocks of the active chain, as it does for peers
            assert_raises_rpc_error(-1, "are not in the same chain", node.protx, "diff", fork_point, block_hash)
        assert_raises_rpc_error(-8, "Base block %s has not been connected" % first, node.protx, "listdiff", first, second)

        self.log.info("masternode payments needs the parent's list: refused for the second block, answered for the first")
        assert_raises_rpc_error(
            -8, "The parent of block %s has not been connected, so there is no masternode list to pay from" % second,
            node.masternode, "payments", second)
        assert_equal(node.masternode("payments", first)[0]["blockhash"], first)
        # a count of 0 reads no list and answers with nothing, for any block
        for block_hash in (first, second, node.getbestblockhash()):
            assert_equal(node.masternode("payments", block_hash, 0), [])

        self.log.info("Before DIP0003 the list is empty by rule: genesis and its child are answered")
        genesis = node.getblockhash(0)
        assert_equal(node.protx("diff", genesis, fork_point)["baseBlockHash"], genesis)
        assert_equal(node.protx("listdiff", 1, fork_point)["baseHeight"], 1)
        assert_equal(node.masternode("payments", node.getblockhash(1))[0]["height"], 1)

        self.log.info("Connected blocks are answered as before")
        tip = node.getbestblockhash()
        assert_equal(node.masternode("payments", tip)[0]["blockhash"], tip)
        assert_equal(node.protx("diff", fork_point, tip)["blockHash"], tip)
        assert_equal(node.protx("listdiff", fork_point, node.getblockcount())["blockHeight"], node.getblockcount())
        assert_raises_rpc_error(-8, "%s not found" % SOME_PROTX, node.protx, "info", SOME_PROTX, tip)

        self.log.info("Blocks connected and then invalidated keep their lists")
        node.invalidateblock(own[0])
        # the other branch has more work now, and the node switches to it
        self.wait_until(lambda: node.getbestblockhash() == second)
        for block_hash in own:
            assert_raises_rpc_error(-8, "%s not found" % SOME_PROTX, node.protx, "info", SOME_PROTX, block_hash)
        assert_equal(node.protx("listdiff", fork_point, own[1])["blockHeight"], fork_point + 2)
        # paid from an invalidated parent's list
        assert_equal(node.masternode("payments", own[1])[0]["blockhash"], own[1])
        # and the branch it switched to, connected now, is answered too
        assert_equal(node.masternode("payments", second)[0]["blockhash"], second)


if __name__ == '__main__':
    MasternodeListsUnconnectedTest().main()
