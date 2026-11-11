#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A -blocksonly node of a wallet build handles the paths of the mixing queue.

A node that does not relay transactions (-blocksonly) has no CoinJoin client
queue manager. Every path that reaches the queue manager must do without it:

  rpc        getcoinjoininfo answers with queue_size 0 and the same fields as
             on a relaying node;
  getdata    a getdata for a DSQ object is answered with notfound;
  getsporks  a getsporks reaches the spork manager;
  spork      an unsigned spork reaches the spork manager, which refuses it;
  sync       a relaying peer's masternode sync sends getsporks on its own, and
             the node then takes the peer's next block;
  inv        a DSQ inv from a peer with the relay permission is looked up and
             requested (any other peer is disconnected for it before that).

After each the node must still be running. --case runs one of them alone (the
negative control runs each on the base).
"""

import struct

from test_framework.messages import (
    CInv,
    msg_getdata,
    msg_inv,
    ser_string,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# src/protocol.h
MSG_DSQ = 32
# src/masternode/sync.h
MASTERNODE_SYNC_TICK_SECONDS = 6
# src/spork.h
SPORK_2_INSTANTSEND_ENABLED = 10001

CASES = ("rpc", "getdata", "getsporks", "spork", "sync", "inv")


class msg_getsporks:
    __slots__ = ()
    msgtype = b"getsporks"

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_getsporks()"


class msg_spork:
    """A spork with an empty signature (src/spork.h, CSporkMessage)."""
    __slots__ = ("spork_id", "value", "time_signed")
    msgtype = b"spork"

    def __init__(self, spork_id, value, time_signed):
        self.spork_id = spork_id
        self.value = value
        self.time_signed = time_signed

    def serialize(self):
        return struct.pack("<iqq", self.spork_id, self.value, self.time_signed) + ser_string(b"")

    def __repr__(self):
        return "msg_spork(id=%d value=%d time_signed=%d)" % (self.spork_id, self.value, self.time_signed)


class P2PBlocksOnlyPeerMessages(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--case", dest="case", default="all", choices=CASES + ("all",),
                            help="run one case only (default: all, in this order)")

    def set_test_params(self):
        self.num_nodes = 2
        # node0 relays transactions and has a queue manager; node1 does not
        self.extra_args = [[], ["-blocksonly"]]

    def skip_test_if_missing_module(self):
        # Without the wallet there is no client queue manager to miss.
        self.skip_if_no_wallet()

    def setup_network(self):
        # Only the sync case connects the two nodes, so every other case reaches node1 alone.
        self.setup_nodes()

    def assert_running(self, case):
        node = self.nodes[1]
        try:
            relay = node.getnetworkinfo()["localrelay"]
        except Exception as e:
            raise AssertionError(f"node1 stopped in the {case} case: {e!r}")
        assert_equal(relay, False)
        assert node.process.poll() is None, f"node1 stopped in the {case} case"

    def case_rpc(self):
        self.log.info("getcoinjoininfo answers on the -blocksonly node, with an empty queue")
        info = self.nodes[1].getcoinjoininfo()
        assert_equal(info["queue_size"], 0)
        assert_equal(sorted(info), sorted(self.nodes[0].getcoinjoininfo()))

    def case_getdata(self):
        self.log.info("A getdata for a DSQ object is answered with notfound")
        peer = self.nodes[1].add_p2p_connection(P2PInterface())
        wanted = 0xd5a1
        peer.send_message(msg_getdata([CInv(t=MSG_DSQ, h=wanted)]))
        peer.wait_until(lambda: "notfound" in peer.last_message and
                        [(i.type, i.hash) for i in peer.last_message["notfound"].vec] == [(MSG_DSQ, wanted)])
        self.nodes[1].disconnect_p2ps()

    def case_getsporks(self):
        self.log.info("A getsporks reaches the spork manager")
        peer = self.nodes[1].add_p2p_connection(P2PInterface())
        peer.send_message(msg_getsporks())
        # A peer's messages are processed in order: the pong comes after the getsporks was handled.
        peer.sync_with_ping()
        self.nodes[1].disconnect_p2ps()

    def case_spork(self):
        self.log.info("An unsigned spork reaches the spork manager, which refuses it")
        peer = self.nodes[1].add_p2p_connection(P2PInterface())
        with self.nodes[1].assert_debug_log(["CSporkManager::ProcessSpork -- ERROR: invalid signature"]):
            peer.send_message(msg_spork(SPORK_2_INSTANTSEND_ENABLED, 0, 0))
        self.nodes[1].disconnect_p2ps()

    def case_sync(self):
        self.log.info("A relaying peer's masternode sync sends getsporks on its own; the node takes its next block")
        self.connect_nodes(1, 0)

        def getsporks_from_node0():
            return any(p["bytesrecv_per_msg"].get("getsporks", 0) > 0
                       for p in self.nodes[1].getpeerinfo() if not p["inbound"])

        # The sync acts once per tick of the mock clock.
        for _ in range(10):
            self.bump_mocktime(MASTERNODE_SYNC_TICK_SECONDS)
            try:
                self.wait_until(getsporks_from_node0, timeout=3)
                break
            except AssertionError:
                continue
        else:
            raise AssertionError("node0 sent no getsporks in ten sync ticks")
        self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks)

    def case_inv(self):
        self.log.info("A DSQ inv from a peer with the relay permission is looked up and requested")
        self.restart_node(1, extra_args=["-blocksonly", "-whitelist=relay@127.0.0.1"])
        peer = self.nodes[1].add_p2p_connection(P2PInterface())
        announced = 0xd5a2
        with self.nodes[1].assert_debug_log([f"got inv: dsq {announced:064x}  new peer=",
                                             f"Requesting dsq {announced:064x} peer="]):
            peer.send_message(msg_inv([CInv(t=MSG_DSQ, h=announced)]))
            peer.sync_with_ping()
        self.nodes[1].disconnect_p2ps()

    def run_test(self):
        assert_equal(self.nodes[1].getnetworkinfo()["localrelay"], False)
        cases = CASES if self.options.case == "all" else (self.options.case,)
        for case in cases:
            getattr(self, "case_" + case)()
            self.assert_running(case)


if __name__ == '__main__':
    P2PBlocksOnlyPeerMessages().main()
