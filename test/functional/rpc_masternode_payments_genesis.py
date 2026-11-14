#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""`masternode payments` at its edges answers, and the node survives each.

Genesis: the RPC fills a payment template from the block's predecessor, which
genesis does not have; the not-null guard on that pointer used to terminate the
daemon. An explorer indexing a fresh regtest chain from height 0 asked exactly
this and took the node down. The answer for genesis is nothing -- no block
before it can be reported -- and every later block still answers as before.

The smallest count, -2**63, has no positive counterpart in a 64-bit integer; it
asks for as many blocks backwards as there are, like any other large negative
count, and the largest count as many forwards.

Without -txindex the fees of a block whose transactions spend outputs of earlier
blocks cannot be summed: a spent transaction is found only while it is in the
mempool. The call says so, instead of reading a transaction it did not find.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class MasternodePaymentsGenesisTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        genesis = node.getblockhash(0)

        self.log.info("Genesis reports no payment, and the daemon is still there afterwards")
        assert_equal(node.masternode("payments", genesis), [])
        assert_equal(node.getblockcount(), 0)

        self.log.info("A walk that would reach genesis stops before it, with the blocks above intact")
        self.generate(node, 3, sync_fun=self.no_op)
        walk = node.masternode("payments", node.getblockhash(3), -10)
        assert_equal([entry["height"] for entry in walk], [1, 2, 3])
        assert_equal(node.getblockcount(), 3)

        self.log.info("The smallest count walks back like any large negative one, the largest forwards")
        walk = node.masternode("payments", node.getblockhash(3), -2**63)
        assert_equal([entry["height"] for entry in walk], [1, 2, 3])
        walk = node.masternode("payments", node.getblockhash(1), 2**63 - 1)
        assert_equal([entry["height"] for entry in walk], [1, 2, 3])

        self.log.info("Without -txindex, a block spending an earlier transaction is refused, saying why")
        self.generate(node, 100, sync_fun=self.no_op)  # the first coinbases mature
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        spending = self.generate(node, 1, sync_fun=self.no_op)[0]
        height = node.getblockcount()
        assert txid in node.getblock(spending)["tx"]
        spent = node.getrawtransaction(txid, True)["vin"][0]["txid"]
        # with the index, the default, the block is answered
        assert_equal(node.masternode("payments", spending)[0]["blockhash"], spending)
        self.restart_node(0, extra_args=["-txindex=0"])
        assert_raises_rpc_error(
            -1, "Transaction %s, spent in block %s, was not found: masternode payments needs -txindex for this block" % (spent, spending),
            node.masternode, "payments", spending)
        assert_equal(node.getblockcount(), height)
        # a block that spends nothing earlier is still answered
        assert_equal(node.masternode("payments", node.getblockhash(3))[0]["height"], 3)


if __name__ == '__main__':
    MasternodePaymentsGenesisTest().main()
