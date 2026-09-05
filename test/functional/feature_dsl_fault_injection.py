#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The DSL fault-injection gate and state model, on regtest.

Two nodes: one started with -enablefaultinjection=1, one without. The armed
node accepts faults, lists exactly the active ones, retires each at its expiry
height and on request, and forgets everything on restart -- there is no
persistence to forget from. The plain node refuses the RPC outright, because
its injector was never enabled. The mainnet startup refusal is a pure function
and lives in the unit tests; regtest cannot pretend to be mainnet here.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

ARMED = "-enablefaultinjection=1"


class DSLFaultInjectionTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[ARMED], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        armed, plain = self.nodes

        self.log.info("A node started without the flag refuses the RPC")
        assert_raises_rpc_error(-1, "fault injection is disabled", plain.faultinject, "list")
        assert_raises_rpc_error(-1, "fault injection is disabled", plain.faultinject, "set", "response-drop", 999, "x")

        self.log.info("The armed node starts clean")
        status = armed.faultinject("list")
        assert_equal(status["enabled"], True)
        assert_equal(status["faults"], [])

        self.log.info("A fault is active below its expiry height and gone at it")
        height = armed.getblockcount()
        fault = armed.faultinject("set", "response-drop", height + 3, "day18-smoke")
        assert_equal(fault["id"], 1)
        assert_equal(fault["kind"], "response-drop")
        assert_equal(fault["setAtHeight"], height)
        assert_equal(fault["expiryHeight"], height + 3)
        assert_equal(fault["scenarioId"], "day18-smoke")
        assert_equal([f["id"] for f in armed.faultinject("list")["faults"]], [1])
        self.generate(armed, 2, sync_fun=self.no_op)
        assert_equal([f["id"] for f in armed.faultinject("list")["faults"]], [1])
        self.generate(armed, 1, sync_fun=self.no_op)
        assert_equal(armed.getblockcount(), height + 3)
        assert_equal(armed.faultinject("list")["faults"], [])

        self.log.info("What could never act is refused")
        now = armed.getblockcount()
        assert_raises_rpc_error(-8, "must be above the current height", armed.faultinject, "set", "response-drop", now, "s")
        assert_raises_rpc_error(-8, "unknown fault kind", armed.faultinject, "set", "drop", now + 5, "s")
        assert_raises_rpc_error(-8, "scenarioId must not be empty", armed.faultinject, "set", "report-drop", now + 5, "")
        assert_raises_rpc_error(-8, "delay kind needs a non-zero param", armed.faultinject, "set", "report-delay", now + 5, "s")
        delayed = armed.faultinject("set", "report-delay", now + 5, "s", 2)
        assert_equal(delayed["param"], 2)

        self.log.info("Clear drops one by id, then all")
        a = armed.faultinject("set", "commitment-skip", now + 50, "a")
        b = armed.faultinject("set", "commitment-skip", now + 60, "b")
        assert a["id"] != b["id"]
        assert_equal(len(armed.faultinject("list")["faults"]), 3)
        assert_equal(armed.faultinject("clear", a["id"])["cleared"], 1)
        assert_equal(armed.faultinject("clear", a["id"])["cleared"], 0)
        assert_equal([f["id"] for f in armed.faultinject("list")["faults"]], [delayed["id"], b["id"]])
        assert_equal(armed.faultinject("clear")["cleared"], 2)
        assert_equal(armed.faultinject("list")["faults"], [])

        self.log.info("A restart forgets every fault: nothing is persisted")
        armed.faultinject("set", "report-drop", armed.getblockcount() + 100, "survives?")
        assert_equal(len(armed.faultinject("list")["faults"]), 1)
        self.restart_node(0, extra_args=[ARMED])
        assert_equal(armed.faultinject("list")["faults"], [])

        self.log.info("Restarted without the flag, the same node refuses the RPC again")
        self.restart_node(0, extra_args=[])
        assert_raises_rpc_error(-1, "fault injection is disabled", armed.faultinject, "list")


if __name__ == '__main__':
    DSLFaultInjectionTest().main()
