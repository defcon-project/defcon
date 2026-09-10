#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The block producer says so when it builds a boundary block without the
epoch's Sentinel commitment (F-2026-140).

The commit decision in BlockAssembler::CreateNewBlock had three outcomes and
logged two: the commitment attached, and the report pool diverging from the
quorum. The third -- no threshold signature by the boundary -- was an `if`
with no `else`, and it is the one the field met (devnet epoch 455, boundary
10944) and could not explain from the journal.

One node, no masternodes, so no quorum ever signs anything: every boundary
the producer is allowed to try at must now leave the line. The first such
boundary is one whole epoch after activation, since the epoch containing the
activation height is never committed (CheckPoSeServiceCommitmentTx's
"commitment-early" rule mirrors it), and the closed epoch it names is the one
that ended there.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

EPOCH_INTERVAL = 24      # Consensus::Params::nDSLEpochInterval, every network
ACTIVATION = 1
FIRST_TRIED_BOUNDARY = 2 * EPOCH_INTERVAL   # 48: nHeight - interval >= activation
TX_TYPE_POSE_SERVICE_COMMITMENT = 10


class DSLCommitDecisionLogTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The producer's DSL block sits inside its DIP3-active section, as on
        # every network that runs the layer; regtest activates DIP3 at 432 by
        # default, far above the boundaries this test looks at.
        self.extra_args = [["-testactivationheight=dsl@%d" % ACTIVATION, "-dip3params=2:2"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Mine to just below the first boundary the producer tries at (%d)", FIRST_TRIED_BOUNDARY)
        self.generate(node, FIRST_TRIED_BOUNDARY - 1)
        assert_equal(node.getblockcount(), FIRST_TRIED_BOUNDARY - 1)

        self.log.info("The boundary block is built without a commitment, and the producer says so")
        closed_epoch = FIRST_TRIED_BOUNDARY // EPOCH_INTERVAL - 1
        with node.assert_debug_log([
            "DSL epoch %d closes at height %d with no threshold signature" % (closed_epoch, FIRST_TRIED_BOUNDARY),
            "block built without the commitment",
        ]):
            block_hash = self.generate(node, 1)[0]
        assert_equal(node.getblockcount(), FIRST_TRIED_BOUNDARY)

        # And the block really carries none: the line describes the block.
        block = node.getblock(block_hash, 2)
        types = [tx.get("type", 0) for tx in block["tx"]]
        assert TX_TYPE_POSE_SERVICE_COMMITMENT not in types, "a commitment was attached after all"

        self.log.info("The next boundary says it again, for the next epoch")
        self.generate(node, EPOCH_INTERVAL - 1)
        with node.assert_debug_log([
            "DSL epoch %d closes at height %d with no threshold signature" % (closed_epoch + 1, FIRST_TRIED_BOUNDARY + EPOCH_INTERVAL),
        ]):
            self.generate(node, 1)


if __name__ == "__main__":
    DSLCommitDecisionLogTest().main()
