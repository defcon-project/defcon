#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The Compute masternode type end to end on regtest.

The type ships dormant behind an activation height. This test brings the gate
into reach with -testactivationheight, proves a registration below the gate is
rejected while one above it lands in the deterministic list with its oracle
descriptor, and renews the service certificate through update_service_compute.
"""

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

ORACLE_KEY = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
COMPUTE_ACTIVATION_HEIGHT = 250


class ComputeMnTest(DashTestFramework):
    def set_test_params(self):
        self.extra_args = [[
            '-testactivationheight=v19@200',
            f'-testactivationheight=compute@{COMPUTE_ACTIVATION_HEIGHT}',
        ]] * 6
        self.set_dash_test_params(6, 5, extra_args=self.extra_args)

    def run_test(self):
        self.log.info("Activating v19")
        self.activate_by_name('v19', expected_activation_height=200)

        height = self.nodes[0].getblockcount()
        assert height < COMPUTE_ACTIVATION_HEIGHT

        self.log.info("A Compute registration below the activation height must be rejected")
        rejected = self.dynamically_add_masternode(compute=True, rnd=7, should_be_rejected=True)
        assert rejected is None

        self.log.info("Reaching the activation height")
        self.bump_mocktime(1)
        self.generate(self.nodes[0], COMPUTE_ACTIVATION_HEIGHT - self.nodes[0].getblockcount())
        assert self.nodes[0].getblockcount() >= COMPUTE_ACTIVATION_HEIGHT

        self.log.info("The same registration above the gate must land in the list")
        mn_info = self.dynamically_add_masternode(compute=True, rnd=7)
        assert mn_info is not None

        info = self.nodes[0].protx('info', mn_info.proTxHash)
        assert_equal(info['type'], 'Compute')
        descriptor = info['state']['computeDescriptor']
        assert_equal(descriptor['oracleKey'], ORACLE_KEY)
        assert_equal(descriptor['endpoint'], 'oracle.example:443')
        assert_equal(descriptor['certExpiryHeight'], 0)

        self.log.info("Renewing the service certificate through update_service_compute")
        cert_hash = '11' * 32
        expiry = self.nodes[0].getblockcount() + 500
        fund_address = self.nodes[0].getnewaddress()
        self.nodes[0].sendtoaddress(fund_address, 1)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(self.nodes[0], 1)
        self.nodes[0].protx('update_service_compute', mn_info.proTxHash, mn_info.addr,
                            mn_info.keyOperator, ORACLE_KEY, 'oracle.example:8443',
                            cert_hash, expiry, "", fund_address)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(self.nodes[0], 1)

        descriptor = self.nodes[0].protx('info', mn_info.proTxHash)['state']['computeDescriptor']
        assert_equal(descriptor['certHash'], cert_hash)
        assert_equal(descriptor['certExpiryHeight'], expiry)
        assert_equal(descriptor['endpoint'], 'oracle.example:8443')

        self.log.info("A regular masternode still registers fine beside it")
        regular = self.dynamically_add_masternode(rnd=8)
        assert regular is not None

        self.log.info("Tests successful")


if __name__ == '__main__':
    ComputeMnTest().main()
