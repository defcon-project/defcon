#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""getstakinginfo names a coin of exactly a CoinJoin denomination amount under excluded.coinjoin (CJ4-B).

The wallet gets a mature 10 000.1 DFCN coin (more duffs than a signed 32-bit integer holds) and a mature coin one duff
larger, both past regtest's minimum stake age; its staking switch is off, so nothing spends them. getstakinginfo reports
exactly 10 000.1 DFCN under coinjoin, and nothing as too young.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

AMOUNT = Decimal("10000.1")  # the largest of the DFCN-scale denominations
MIN_STAKE_AGE = 600          # regtest's stakeAgeRange[0]


class WalletStakingCoinJoinAmountTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.wait_until(lambda: len(node.liststakingwallets()) > 0, timeout=30)
        wallet_id = next(iter(node.liststakingwallets()))
        if node.getstakinginfo()[wallet_id]["staking"]:
            assert_equal(node.setstaking(int(wallet_id)), False)

        self.log.info("A mature 10 000.1 DFCN coin and one a duff larger, past the minimum age: only the first is excluded")
        node.sendmany("", {node.getnewaddress(): AMOUNT, node.getnewaddress(): AMOUNT + Decimal("0.00000001")})
        funding_time = node.getblock(self.generate(node, 1)[0])["time"]
        # The maturity blocks first, then the time past the minimum age and a new tip: every coin mature by depth (the
        # two, the change and the older block rewards) is then also old enough, so nothing is too young.
        self.generate(node, COINBASE_MATURITY + 3)
        self.bump_mocktime(MIN_STAKE_AGE + 1)
        tip = self.generate(node, 1)[0]
        assert node.getblock(tip)["time"] - funding_time >= MIN_STAKE_AGE
        excluded = node.getstakinginfo()[wallet_id]["excluded"]
        assert_equal(excluded["coinjoin"], AMOUNT)  # a JSON number, decoded as a Decimal
        assert "too_young" not in excluded


if __name__ == '__main__':
    WalletStakingCoinJoinAmountTest().main()
