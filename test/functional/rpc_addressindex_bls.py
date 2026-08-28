#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Address-index RPCs must reject a pay-to-BLS-pubkey address, not dereference it.

DecodeDestination turns a BLS address into a CPubKey destination -- the variant
member the fork added to CTxDestination. IsValidDestination accepts it (its index
is not zero), but it is neither PKHash nor ScriptHash, so both std::get_if calls
in getIndexKey() answer nullptr and the second one used to be dereferenced.

Such outputs are never written to the address index in the first place:
AddressBytesFromScript() only recognises P2SH, P2PKH and P2PK and answers UNKNOWN
for everything else, and the indexer skips UNKNOWN. There is therefore nothing to
look up, and the honest answer is "Invalid address".
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error

# base58check(premine BLS pubkey), no version prefix -- see EncodeDestination(CPubKey)
BLS_PREMINE_ADDRESS = "ZHSyHDAvTeh1epfmjoP9w1WQ6jQoaaA5abSXD7uRpGj2ZTBRF3wJvhuszJEdaweqQWj2AwG"

ADDRESS_INDEX_RPCS = (
    "getaddressmempool",
    "getaddressutxos",
    "getaddressdeltas",
    "getaddressbalance",
    "getaddresstxids",
)


class AddressIndexBLSAddressTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-addressindex"]]

    def run_test(self):
        node = self.nodes[0]

        for rpc_name in ADDRESS_INDEX_RPCS:
            self.log.info(f"{rpc_name} with a BLS address must answer, not die")
            assert_raises_rpc_error(-5, "Invalid address", getattr(node, rpc_name),
                                    {"addresses": [BLS_PREMINE_ADDRESS]})
            # Ask something trivial straight after: before the guard the RPC above
            # dereferenced a null pointer and took the node with it, so a live
            # answer here is the real assertion.
            node.getbestblockhash()

        self.log.info("A single-string argument takes the same path and must behave the same")
        assert_raises_rpc_error(-5, "Invalid address", node.getaddresstxids, BLS_PREMINE_ADDRESS)
        node.getbestblockhash()


if __name__ == '__main__':
    AddressIndexBLSAddressTest().main()
