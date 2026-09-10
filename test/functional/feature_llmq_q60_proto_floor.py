#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The peer protocol floor moves at the Q60 formation lead.

From activation - 120 on, a node refuses peers below Q60_SWITCHOVER_PROTO_VERSION
-- at the version handshake for new connections, and from SendMessages for
the ones already up, the way the fork-recovery floor has always worked
(src/net_processing.cpp: GetRequiredPeerProtocolVersion, and its two callers).

Why the lead and not the activation height: the first llmq_defcon commitment
is mined inside the lead, and a binary that does not know the profile forks
off there. A peer still speaking the old protocol after that point is on the
old chain by construction, and everything it relays -- headers, blocks, the
legacy ChainLocks the pause in llmq::IsChainLockPaused refuses one layer down
-- is old-chain data. Dropping the connection is cheaper than filtering it.

The old peer advertises 70241 -- not a made-up number one below the floor,
but the version every v22.1.x binary really speaks (MN_DSL_PROTO_VERSION,
7bf3c5ad19). The first version of this floor sat at 70241 too, so the
predecessor was admitted and the synthetic 70240 control never noticed; the
independent review did, with an unmodified predecessor build. -pushversion,
which the node honours on every network but mainnet, presents that version
here; a current-version peer stands beside it as the control: the floor must
remove exactly one of the two, and the node under test must itself advertise
the floor, or the assertion in version.h that keeps the two apart is moot.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port

ACTIVATION = 240
LEAD = 120                 # (signingActiveQuorumCount + 1) * dkgInterval of llmq_defcon
FLOOR_HEIGHT = ACTIVATION - LEAD
FLOOR = 70242              # Q60_SWITCHOVER_PROTO_VERSION, src/version.h
OLD = 70241                # LAST_PRE_SWITCHOVER_PROTO_VERSION: what every v22.1.x binary advertises
assert OLD < FLOOR


class LLMQQ60ProtoFloorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        switchover = "-testactivationheight=chainlocksv2@%d" % ACTIVATION
        self.extra_args = [
            [switchover],                              # node 0: the node under test
            [switchover, "-pushversion=%d" % OLD],     # node 1: one below the floor
            [switchover],                              # node 2: the control, current version
        ]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)

    def peer_versions(self, node):
        return sorted(p["version"] for p in node.getpeerinfo())

    def run_test(self):
        node, old, new = self.nodes

        self.log.info("The node under test advertises the floor itself, above every v22.1.x binary")
        assert_equal(node.getnetworkinfo()["protocolversion"], FLOOR)
        # (the old node's getnetworkinfo would answer FLOOR too: the RPC reports
        # the compiled PROTOCOL_VERSION, and -pushversion only changes what is
        # sent on the wire -- which is what the peer list below observes)

        self.log.info("Below the lead both peers are accepted, the old one included")
        assert_equal(self.peer_versions(node), sorted([OLD, FLOOR]))

        self.generate(node, FLOOR_HEIGHT - 1, sync_fun=self.sync_blocks)
        assert_equal(node.getblockcount(), FLOOR_HEIGHT - 1)
        assert_equal(self.peer_versions(node), sorted([OLD, FLOOR]))

        self.log.info("At the lead (%d) the floor moves and the old peer is dropped, the current one kept",
                      FLOOR_HEIGHT)
        with node.assert_debug_log(["using obsolete version %d" % OLD]):
            self.generate(node, 1, sync_fun=self.no_op)
            assert_equal(node.getblockcount(), FLOOR_HEIGHT)
            self.wait_until(lambda: self.peer_versions(node) == [FLOOR], timeout=30)

        self.log.info("The old peer cannot come back")
        with node.assert_debug_log(["using obsolete version %d" % OLD]):
            old.addnode("127.0.0.1:%d" % p2p_port(0), "onetry")
            # A negative wait: two peers must NOT reappear.
            assert not self.wait_until(lambda: len(node.getpeerinfo()) == 2, timeout=5, do_assert=False)
        assert_equal(self.peer_versions(node), [FLOOR])

        self.log.info("The current-version peer keeps following the chain across the lead")
        self.generate(node, 3, sync_fun=self.no_op)
        self.sync_blocks([node, new])
        assert_equal(new.getblockcount(), FLOOR_HEIGHT + 3)
        # The old peer is stranded at the cut. Whether it still got the block
        # that moved the floor is a race with the disconnect and not the claim;
        # the claim is that nothing mined after the cut reaches it.
        assert old.getblockcount() <= FLOOR_HEIGHT, "the old peer followed the chain past the floor"


if __name__ == "__main__":
    LLMQQ60ProtoFloorTest().main()
