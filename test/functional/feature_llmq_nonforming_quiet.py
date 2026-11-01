#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A masternode logs no failure for a quorum type that does not form.

The network has one masternode. llmq_test_instantsend never forms on regtest and
llmq_test_v17 stops forming at END. llmq_test does form, and one member is too few
for it: that is a real failure, so it keeps its line and shows the round was reached.

1. The last cycle below END: llmq_test_v17 still fails like llmq_test.
2. The cycle at END and the next one: llmq_test_v17 is quiet too, and its thread keeps
   going from cycle to cycle.
3. 'quorum dkginfo' no longer lists the cycle at END for llmq_test_v17, before its
   predecessor is mined, and next_dkg still follows the types that form.
"""

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than

END = 720              # on llmq_test_v17's 24-block grid, below regtest's V20 height
INTERVAL = 24
V17_TYPE = 102
TEST_TYPE = 100
NEVER_FORMS = "llmq_test_instantsend"
RETIRED = "llmq_test_v17"
TOO_FEW = "llmq_test"


class LLMQNonFormingQuietTest(DashTestFramework):
    def set_test_params(self):
        args = ["-vbparams=testdummy:0:999999999999:0:10:8:6:5:0",
                f"-llmqformationendheight={RETIRED}:{END}"]
        self.set_dash_test_params(2, 1, extra_args=[args] * 2)

    def generate_to(self, height):
        tip = self.nodes[0].getblockcount()
        assert height >= tip, (height, tip)
        while tip < height:
            n = min(10, height - tip)
            self.bump_mocktime(n)
            self.generate(self.nodes[0], n)
            tip += n
        assert_equal(self.nodes[0].getblockcount(), height)

    def failed_line(self, base, name):
        return f"height[{base}] quorum initialization failed for {name} qi[0]"

    def check_cycle(self, base, quiet, failing):
        """Stop on the cycle's base block, where every handler thread starts its round."""
        self.log.info(f"cycle at {base}: quiet {quiet}, failing {failing}")
        self.generate_to(base)
        expected = [self.failed_line(base, name) for name in failing]
        expected += [f"height[{base}] {name} qi[0] does not form" for name in quiet]

        def reached():
            with open(self.mninfo[0].node.debug_log_path, encoding='utf-8') as dl:
                log = dl.read()
            for name in quiet:
                assert self.failed_line(base, name) not in log, self.failed_line(base, name)
            return all(line in log for line in expected)
        self.wait_until(reached)

    def upcoming(self, llmq_type):
        """The heights 'quorum dkginfo' lists for a type; the tip must be past the work block."""
        dkgs = self.nodes[0].quorum("dkginfo", self.mninfo[0].proTxHash)["upcoming_dkgs"]
        return [d["quorumHeight"] for d in dkgs if d["llmqType"] == llmq_type]

    def run_test(self):
        node = self.nodes[0]
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        start = node.getblockcount()
        self.log.info(f"height after setup {start}, END {END}")
        assert_greater_than(END - INTERVAL - 8, start)

        self.log.info("the cycle below END is listed for the type it retires")
        self.generate_to(END - INTERVAL - 5)
        assert_equal(self.upcoming(V17_TYPE), [END - INTERVAL])
        assert_equal(self.upcoming(TEST_TYPE), [END - INTERVAL])

        self.check_cycle(END - INTERVAL, quiet=[NEVER_FORMS], failing=[RETIRED, TOO_FEW])

        self.log.info("the cycle at END is not, though its predecessor is not mined yet")
        self.generate_to(END - 5)
        assert_equal(self.upcoming(V17_TYPE), [])
        assert_equal(self.upcoming(TEST_TYPE), [END])
        assert_equal(node.quorum("dkginfo")["next_dkg"], 5)

        self.check_cycle(END, quiet=[NEVER_FORMS, RETIRED], failing=[TOO_FEW])
        self.check_cycle(END + INTERVAL, quiet=[NEVER_FORMS, RETIRED], failing=[TOO_FEW])

        self.log.info("next_dkg: the rotating llmq_test_dip0024 forms, its first sessions count")
        assert_equal(node.quorum("dkginfo")["next_dkg"], 1)
        self.generate_to(END + INTERVAL + 5)
        assert_equal(node.quorum("dkginfo")["next_dkg"], INTERVAL - 5)


if __name__ == '__main__':
    LLMQNonFormingQuietTest().main()
