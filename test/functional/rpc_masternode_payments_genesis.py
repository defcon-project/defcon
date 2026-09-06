#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""`masternode payments` at genesis answers, and the node survives it.

The RPC fills a payment template from the block's predecessor, which genesis
does not have; the not-null guard on that pointer used to terminate the daemon.
An explorer indexing a fresh regtest chain from height 0 asked exactly this and
took the node down. The answer for genesis is nothing -- no block before it can
be reported -- and every later block still answers as before.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


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


if __name__ == '__main__':
    MasternodePaymentsGenesisTest().main()
