#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A quorum profile that stops forming at a height (-llmqformationendheight).

llmq_test_v17 is retired at END on every node; it holds no role on regtest, and
llmq_test (ChainLocks, MNHF) keeps forming as the control.

1. Below END the profile forms a real quorum and is listed as enabled.
2. The gate turns at exactly END: enabled while the next block is END-1, not
   once the next block is END.
3. On the chain: the last cycle below END mines its commitment inside its
   mining window, before END; the first cycle at END mines none, while
   llmq_test's cycle at END still does.
4. A reorg back below END and a new chain across it give the same picture.
"""

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than

END = 720              # on llmq_test_v17's 24-block grid, below regtest's V20 height
V17_TYPE = 102
TEST_TYPE = 100
QUORUM_COMMITMENT_TX = 6
WINDOW = (10, 18)      # dkgMiningWindowStart/End of both profiles


class LLMQFormationEndTest(DashTestFramework):
    def set_test_params(self):
        args = ["-vbparams=testdummy:0:999999999999:0:10:8:6:5:0",
                f"-llmqformationendheight=llmq_test_v17:{END}"]
        self.set_dash_test_params(5, 4, extra_args=[args] * 5)

    def commitment_types(self, height):
        """The LLMQ types of the commitments mined in the block at this height."""
        node = self.nodes[0]
        block = node.getblock(node.getblockhash(height), 2)
        return [tx["qcTx"]["commitment"]["llmqType"] for tx in block["tx"]
                if tx.get("type") == QUORUM_COMMITMENT_TX and "qcTx" in tx]

    def types_in_window(self, base):
        found = []
        for h in range(base + WINDOW[0], base + WINDOW[1] + 1):
            found += self.commitment_types(h)
        return found

    def listed(self):
        return "llmq_test_v17" in self.nodes[0].quorum("list")

    def generate_to(self, height):
        tip = self.nodes[0].getblockcount()
        assert height >= tip, (height, tip)
        while tip < height:
            n = min(10, height - tip)
            self.bump_mocktime(n)
            self.generate(self.nodes[0], n)
            tip += n
        assert_equal(self.nodes[0].getblockcount(), height)

    def check_across_the_end(self, label):
        self.log.info(f"{label}: the last cycle below END mined its llmq_test_v17 commitment before END")
        last_below = END - 24
        assert V17_TYPE in self.types_in_window(last_below), self.types_in_window(last_below)
        self.log.info(f"{label}: the cycle at END mined none, while llmq_test's did")
        at_end = self.types_in_window(END)
        assert V17_TYPE not in at_end, at_end
        assert TEST_TYPE in at_end, at_end
        # nothing of the retired profile anywhere past END
        for h in range(END, self.nodes[0].getblockcount() + 1):
            assert V17_TYPE not in self.commitment_types(h), h

    def run_test(self):
        node = self.nodes[0]
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        # No ChainLocks: step 4 reorgs the chain back below END, and an enforced
        # lock would put the locked tip straight back.
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        start = node.getblockcount()
        self.log.info(f"height after setup {start}, END {END}")
        assert_greater_than(END - 72, start)

        self.log.info("1. below END: llmq_test_v17 is enabled and forms a real quorum")
        assert self.listed()
        before = len(node.quorum("list")["llmq_test_v17"])
        self.mine_quorum(llmq_type_name="llmq_test_v17", llmq_type=V17_TYPE)
        assert_greater_than(len(node.quorum("list")["llmq_test_v17"]), before)

        self.log.info("2. the gate turns at exactly END")
        self.generate_to(END - 2)
        assert self.listed()                 # next block END-1: still forming
        self.generate_to(END - 1)
        assert not self.listed()             # next block is END: retired
        self.generate_to(END + 30)
        assert not self.listed()

        self.check_across_the_end("3. first chain")

        self.log.info("4. reorg: every node back below END, then a new chain across it")
        fork_height = END - 23               # re-mines the last cycle's window too
        fork_hash = node.getblockhash(fork_height)
        for n in self.nodes:
            n.invalidateblock(fork_hash)
        self.wait_until(lambda: all(n.getblockcount() == fork_height - 1 for n in self.nodes))
        assert self.listed()
        self.generate_to(END + 30)
        self.sync_blocks()
        assert_equal(len(set(n.getbestblockhash() for n in self.nodes)), 1)
        assert node.getbestblockhash() != fork_hash
        self.check_across_the_end("4. new chain")
        assert not self.listed()


if __name__ == '__main__':
    LLMQFormationEndTest().main()
