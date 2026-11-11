#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""connect_nodes reaches a masternode that another node connected to first.

A restarted masternode is dialled by the other nodes' masternode threads as soon as it listens. Once node 0 has
authenticated the masternode on such a connection, mnauth drops the connection the test then makes between the two as
a duplicate, and a connect_nodes that does not close the earlier one first waits for a connection that is gone
(feature_dsl_dkg_isolation.py met this order by chance on the CI). DashTestFramework.connect_nodes closes it first.

Here that order is made on purpose. The setup is feature_dsl_service.py's (seven masternodes and a producer, one
llmq_test quorum); a valid member is restarted, and the test waits until node 0 holds an authenticated, outbound, not
manual connection to it before it calls connect_nodes(member, 0):
 - first it lets node 0's own masternode thread make that connection (--thread-wait seconds, the clock moving);
 - if the thread does not (node 0 watches only some members), node 0 opens the same kind of connection itself
   (addconnection outbound-full-relay), which the member authenticates on in the same way.
Then connect_nodes(member, 0): the member must hold exactly one manual connection to node 0, and take the next block.
"""

from feature_dsl_service import DSLServiceTest
from test_framework.util import assert_equal, force_finish_mnsync, p2p_port

LLMQ_TEST = 100


class MasternodeConnectRaceTest(DSLServiceTest):
    def set_test_params(self):
        super().set_test_params()  # the framework wants it overridden in the test class itself

    def add_options(self, parser):
        parser.add_argument("--thread-wait", dest="thread_wait", type=int, default=15,
                            help="seconds to give node 0's masternode thread before the test opens the connection itself")

    def run_test(self):
        producer = self.nodes[0]
        producer.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        for node in self.nodes:
            force_finish_mnsync(node)
        quorum_hash = self.mine_quorum()
        members = producer.quorum("info", LLMQ_TEST, quorum_hash)["members"]
        valid = [m["proTxHash"] for m in members if m["valid"]]
        mn = next(mn for mn in self.mninfo if mn.proTxHash in valid)
        addr = "127.0.0.1:%d" % p2p_port(mn.nodeIdx)
        self.log.info("Restarting node %d, a valid member of quorum %s", mn.nodeIdx, quorum_hash)
        self.stop_node(mn.nodeIdx)
        self.start_masternode(mn)
        self.bump_mocktime(1)

        def earlier():
            return [p for p in producer.getpeerinfo()
                    if p["addr"] == addr and not p["inbound"] and p["connection_type"] != "manual"]

        def authenticated():
            return any(p.get("verified_proregtx_hash") == mn.proTxHash for p in earlier())

        def by_thread():
            self.bump_mocktime(1)
            return authenticated()
        try:
            self.wait_until(by_thread, timeout=self.options.thread_wait / self.options.timeout_factor, sleep=0.5)
            self.log.info("node 0's masternode thread connected to node %d first and authenticated it", mn.nodeIdx)
        except AssertionError:
            self.log.info("node 0's masternode thread did not connect to node %d: node 0 opens the connection itself", mn.nodeIdx)
            if not earlier():
                producer.addconnection(addr, "outbound-full-relay", False)
            self.wait_until(authenticated, timeout=30)
        self.log.info("node 0 holds %d earlier connection(s) to node %d: %s", len(earlier()), mn.nodeIdx,
                      [(p["id"], p["connection_type"], p.get("masternode")) for p in earlier()])

        self.log.info("connect_nodes(%d, 0)", mn.nodeIdx)
        self.connect_nodes(mn.nodeIdx, 0)
        member = self.nodes[mn.nodeIdx]
        manual = [p for p in member.getpeerinfo()
                  if p["addr"] == "127.0.0.1:%d" % p2p_port(0) and not p["inbound"] and p["connection_type"] == "manual"]
        assert_equal(len(manual), 1)
        self.sync_blocks()
        self.bump_mocktime(1)
        self.generate(producer, 1)
        assert_equal(member.getbestblockhash(), producer.getbestblockhash())
        self.log.info("connected, and the member took the next block")


if __name__ == '__main__':
    MasternodeConnectRaceTest().main()
