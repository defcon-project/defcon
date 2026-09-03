#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_is_nonrotation.py

A mempool transaction becomes InstantSend-locked within seconds, without a
block, on a quorum that does not rotate. This is the configuration this chain
runs -- DIP0024 never activated, so every InstantSend profile here is
non-rotating -- and it is what p2p_instantsend.py does not cover, because that
test locks on the regtest rotation quorum.

The lock is checked on `instantlock_internal`, never on `instantlock`: the
latter is also true for a ChainLocked block, so it can pass with no ISLOCK at
all.

A locked transaction is mined immediately. One that cannot be locked waits
WAIT_FOR_ISLOCK_TIMEOUT (two minutes), not the ten this chain inherited.
'''

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

LLMQ_TYPE_TEST_INSTANTSEND = 104
DKG_INTERVAL = 24
WAIT_FOR_ISLOCK_TIMEOUT = 2 * 60


class LLMQInstantSendNonRotationTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(5, 4, [["-llmqtestinstantsenddip0024=llmq_test_instantsend"]] * 5)
        self.set_dash_llmq_test_params(4, 3)

    def run_test(self):
        node = self.nodes[0]

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        # The mining wait exists only while block filtering is on; regtest
        # sporks default to off.
        self.nodes[0].sporkupdate("SPORK_3_INSTANTSEND_BLOCK_FILTERING", 0)
        self.wait_for_sporks_same()

        self.log.info("Form the non-rotating InstantSend quorum")
        self.mine_quorum(llmq_type_name="llmq_test_instantsend", llmq_type=LLMQ_TYPE_TEST_INSTANTSEND)

        self.log.info("A mempool transaction is locked without a block")
        height = node.getblockcount()
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        self.sync_mempools()
        for n in self.nodes:
            self.wait_for_internal_instantlock(txid, n)
        assert_equal(node.getblockcount(), height)

        self.log.info("The lock is visible through every RPC that reports it")
        islocks = node.getislocks([txid])
        assert_equal(len(islocks), 1)
        assert_equal(islocks[0]["txid"], txid)
        assert_equal(islocks[0]["inputs"][0]["txid"], node.getrawtransaction(txid, True)["vin"][0]["txid"])
        cycle_height = node.getblock(islocks[0]["cycleHash"])["height"]
        assert_equal(cycle_height % DKG_INTERVAL, 0)
        assert_equal(node.getmempoolentry(txid)["instantlock"], "true")
        wtx = node.gettransaction(txid)
        assert_equal(wtx["instantlock"], True)
        assert_equal(wtx["instantlock_internal"], True)
        assert_equal(wtx["confirmations"], 0)

        self.log.info("A locked transaction is mined at once, however young it is")
        block = self.generate(node, 1)[0]
        assert txid in node.getblock(block, 1)["tx"]

        self.log.info("An unlockable transaction waits WAIT_FOR_ISLOCK_TIMEOUT, not ten minutes")
        # A spork value of 1 keeps InstantSend enabled but disables mempool
        # signing, so this transaction stays unlocked on purpose.
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 1)
        self.wait_for_sporks_same()
        unlocked = node.sendtoaddress(node.getnewaddress(), 1)
        self.sync_mempools()
        # The wait is measured from the mempool acceptance time; the sync
        # above bumps mocktime as it polls, so aim from that time, not from now.
        accepted = node.getmempoolentry(unlocked)["time"]
        self.bump_mocktime(accepted + WAIT_FOR_ISLOCK_TIMEOUT - 1 - self.mocktime)
        early = self.generate(node, 1)[0]
        assert unlocked not in node.getblock(early, 1)["tx"]
        self.bump_mocktime(1)
        late = self.generate(node, 1)[0]
        assert unlocked in node.getblock(late, 1)["tx"]

    def wait_for_internal_instantlock(self, txid, node, timeout=15):
        def locked():
            self.bump_mocktime(1)
            try:
                return node.getrawtransaction(txid, True)["instantlock_internal"]
            except Exception:
                return False
        self.wait_until(locked, timeout=timeout, sleep=1)


if __name__ == '__main__':
    LLMQInstantSendNonRotationTest().main()
