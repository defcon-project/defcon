#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Why staking does not go on for a wallet is in the log without -debug=pos.

The GUI points to the log when staking cannot be switched on for a wallet. The reasons were logged only under
-debug=pos; now they are logged at the default level. The node runs with -debugexclude=pos, since the test framework
turns every category on: only lines logged without the category show.

An encrypted, locked descriptor wallet: setstaking refuses to switch staking on, and the log says the wallet is locked.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

LOCKED_MSG = "EnsureCoinstakeDescriptors: wallet is locked; unlock it before enabling staking"


class PosStakingRefusalLogTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=1", "-debugexclude=pos"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]
        assert node.getwalletinfo()["descriptors"]
        self.wait_until(lambda: len(node.liststakingwallets()) > 0, timeout=30)
        wallet_id = int(next(iter(node.liststakingwallets())))
        node.encryptwallet("refused for now")
        self.log.info("A locked descriptor wallet: staking stays off, and the log says why")
        with node.assert_debug_log([LOCKED_MSG]):
            assert_equal(node.setstaking(wallet_id), False)
        assert_equal(node.getstakinginfo()[str(wallet_id)]["staking"], False)


if __name__ == '__main__':
    PosStakingRefusalLogTest().main()
