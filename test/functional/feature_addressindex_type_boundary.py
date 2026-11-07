#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A P2PKH and a P2SH record with the same hash stay apart when they are neighbours on disk.

The address index orders its keys by address type first and by hash second, so
the last P2PKH record (hash ff..ff) is followed on disk by the P2SH record with
the smallest hash. A reader that stops only when the hash changes runs on into
that P2SH record if it has the same hash, which is the case exactly when no
other P2SH output is indexed. feature_addressindex.py checks the same pair on a
chain that already holds other P2SH outputs; there the two records are not
adjacent, and such a reader stops anyway. This test keeps the index sparse on
purpose: one node, coinbases to P2PKH addresses, and one transaction that pays
the hash ff..ff once as a P2PKH and once as a P2SH output (#328).
"""

from test_framework.address import keyhash_to_p2pkh, scripthash_to_p2sh
from test_framework.messages import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# consensus/consensus.h: generated outputs mature after 25 blocks on regtest
COINBASE_MATURITY = 25


class AddressIndexTypeBoundaryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-addressindex"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generatetoaddress(node, COINBASE_MATURITY + 1, node.getnewaddress())

        max_hash = bytes.fromhex("ff" * 20)
        p2pkh = keyhash_to_p2pkh(max_hash)
        p2sh = scripthash_to_p2sh(max_hash)

        self.log.info("Pay the hash ff..ff once as P2PKH and once as P2SH; the P2SH output is the only one in the index")
        txid = node.sendmany("", {p2pkh: 1, p2sh: 1})
        self.generate(node, 1)
        height = node.getblockcount()

        self.log.info("Each type answers for its own record only")
        for address in (p2pkh, p2sh):
            assert_equal(node.getaddresstxids({"addresses": [address]}), [txid])

            deltas = node.getaddressdeltas({"addresses": [address]})
            assert_equal(len(deltas), 1)
            assert_equal(deltas[0]["address"], address)
            assert_equal(deltas[0]["satoshis"], COIN)
            assert_equal(deltas[0]["height"], height)

            ranged = node.getaddressdeltas({"addresses": [address], "start": height, "end": height})
            assert_equal(len(ranged), 1)
            assert_equal(ranged[0]["address"], address)

            utxos = node.getaddressutxos({"addresses": [address]})
            assert_equal(len(utxos), 1)
            assert_equal(utxos[0]["address"], address)
            assert_equal(utxos[0]["satoshis"], COIN)

            assert_equal(node.getaddressbalance({"addresses": [address]})["balance"], COIN)


if __name__ == '__main__':
    AddressIndexTypeBoundaryTest().main()
