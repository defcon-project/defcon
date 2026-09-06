#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Operator-signed RPCs must work for legacy MNs across V19 (F-028/F-026).

The fork does not migrate stored MN versions at activation. RPC payloads must
nevertheless use the deployment's BLS scheme, which validation uses globally.
Check both RPCs before/after activation, mining and deterministic-list state,
and a post-activation basic MN control, without requiring a running MN mesh.
"""

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, force_finish_mnsync, p2p_port, softfork_active


V19_HEIGHT = 200


class DIP3V19LegacyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-dip3params=2:2", f"-testactivationheight=v19@{V19_HEIGHT}"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, blocks=1):
        # Make fee-funding and special transactions old enough for the miner.
        self.bump_mocktime(601 + blocks)
        return self.generate(self.nodes[0], blocks)

    def check_mined(self, txid, payload_name, version):
        node = self.nodes[0]
        assert txid in node.getrawmempool()
        blockhash = self.mine()[0]
        tx = node.getrawtransaction(txid, True, blockhash)
        assert_equal(tx["confirmations"], 1)
        assert_equal(tx[payload_name]["version"], version)
        self.log.info(f"Mined {payload_name} version={version} height={node.getblockcount()} txid={txid}")
        return tx[payload_name]

    def register_mn(self, index, legacy):
        node = self.nodes[0]
        key = node.bls("generate", legacy)
        owner = node.getnewaddress()
        collateral = node.getnewaddress()
        service = f"127.0.0.1:{p2p_port(index + 1)}"
        version = 1 if legacy else 2
        txid = node.protx(
            "register_fund_legacy" if legacy else "register_fund",
            collateral, service, owner, key["public"], owner, 0,
            node.getnewaddress(), self.funds[index],
        )
        payload = self.check_mined(txid, "proRegTx", version)
        # register_fund locks its collateral; fixture funding must not spend it.
        assert {"txid": txid, "vout": payload["collateralIndex"]} in node.listlockunspent()
        state = node.protx("info", txid)["state"]
        assert_equal(state["version"], version)
        assert_equal(state["pubKeyOperator"], key["public"])
        assert_equal(state["PoSeBanHeight"], -1)
        return {"protx": txid, "key": key, "version": version, "funds": self.funds[index], "port": p2p_port(index + 1)}

    def update_service(self, mn, payload_version, host):
        node = self.nodes[0]
        service = f"{host}:{mn['port']}"
        txid = node.protx("update_service", mn["protx"], service, mn["key"]["secret"], "", mn["funds"])
        payload = self.check_mined(txid, "proUpServTx", payload_version)
        assert_equal(payload["proTxHash"], mn["protx"])
        assert_equal(payload["service"], service)
        state = node.protx("info", mn["protx"])["state"]
        assert_equal(state["service"], service)
        assert_equal(state["version"], mn["version"])
        assert_equal(state["pubKeyOperator"], mn["key"]["public"])
        assert_equal(state["PoSeBanHeight"], -1)

    def revoke(self, mn, payload_version):
        node = self.nodes[0]
        txid = node.protx("revoke", mn["protx"], mn["key"]["secret"], 1, mn["funds"])
        payload = self.check_mined(txid, "proUpRevTx", payload_version)
        assert_equal(payload["proTxHash"], mn["protx"])
        assert_equal(payload["reason"], 1)
        state = node.protx("info", mn["protx"])["state"]
        # Existing ResetOperatorFields resets even a basic MN to legacy/empty.
        assert_equal(state["version"], 1)
        assert_equal(state["revocationReason"], 1)
        assert_equal(state["PoSeBanHeight"], node.getblockcount())
        assert_equal(state["pubKeyOperator"], "00" * 48)

    def run_test(self):
        node = self.nodes[0]
        self.mine(110)
        force_finish_mnsync(node)
        self.funds = [node.getnewaddress() for _ in range(3)]
        node.sendmany("", {address: 2000 for address in self.funds})
        self.mine()
        assert not softfork_active(node, "v19")

        self.log.info("Register two legacy MNs below V19; update/revoke controls use version 1")
        legacy = self.register_mn(0, legacy=True)
        control = self.register_mn(1, legacy=True)
        self.update_service(legacy, payload_version=1, host="127.0.0.2")
        self.update_service(control, payload_version=1, host="127.0.0.2")
        self.revoke(control, payload_version=1)
        assert node.getblockcount() < V19_HEIGHT - 1
        assert not softfork_active(node, "v19")

        self.mine(V19_HEIGHT - 2 - node.getblockcount())
        assert_equal(node.getblockcount(), V19_HEIGHT - 2)
        assert not softfork_active(node, "v19")
        self.mine()
        # RPC reports activation for the next block, so tip N-1 is the boundary.
        assert_equal(node.getblockcount(), V19_HEIGHT - 1)
        assert softfork_active(node, "v19")
        state = node.protx("info", legacy["protx"])["state"]
        assert_equal(state["version"], 1)
        assert_equal(state["pubKeyOperator"], legacy["key"]["public"])

        self.log.info("Post-V19 legacy MN: update/revoke must mine version 2 payloads without state migration")
        failures = []
        # Exercise both broken RPCs on an unfixed daemon, not just the first.
        # Any RPC error still fails the test after the independent controls.
        for rpc in ("update_service", "revoke"):
            try:
                if rpc == "update_service":
                    self.update_service(legacy, payload_version=2, host="127.0.0.3")
                else:
                    self.revoke(legacy, payload_version=2)
            except JSONRPCException as error:
                failures.append((rpc, error.error))
                self.log.error(f"Post-V19 legacy {rpc} rejected: {error.error}")

        self.log.info("A newly registered basic MN also updates and revokes with version 2")
        basic = self.register_mn(2, legacy=False)
        self.update_service(basic, payload_version=2, host="127.0.0.2")
        self.revoke(basic, payload_version=2)
        assert_equal(failures, [])


if __name__ == '__main__':
    DIP3V19LegacyTest().main()
