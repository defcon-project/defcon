#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A way back from a deep branch: headers from a trusted peer, and a warning when outbound peers offer such a chain.

A node takes headers without their blocks only on a branch that leaves its active chain at most
MAX_UNBACKED_FORK_DEPTH blocks below its tip (p2p_unbacked_pos_headers.py, step 5), so a node left on a branch the
network abandoned deeper than that does not move on its own. Now:

- a peer added by hand (addnode) or with the noban permission is not held to the depth limit (the height limit and
  the branch count still apply to it), so one trusted peer brings the node back;
- headers the depth limit drops from an outbound peer that offer more work than the tip (or fill their message) are
  logged without -debug, at most once an hour, and turn on a node warning (getnetworkinfo "warnings", the GUI), which
  clears a day after the last such headers.

The depth is DEPTH here (-testunbackedforkdepth, regtest only), so the chain needs only a few proof-of-stake blocks.
The noban peer connects to a second listening port that grants the permission (-whitebind), so one node serves both
kinds of peer without a restart.

 h1. A branch of headers offering more work, off DEPTH + 1 blocks below the tip, from an ordinary inbound peer: not
     taken; no warning and no log line (an inbound peer, whatever it offers).
 h2. A short branch off the same block from a noban peer: taken (headers only); the active chain does not change.
 c1. A compact block on a branch whose first header was taken while it was shallow, after the chain grew past the
     depth limit, from an ordinary peer: its header is not taken.
 c2. The same compact block from a noban peer: its header is taken, its block (less work) is not.
 w1. A branch offering more work, off DEPTH + 1 blocks below the tip, from an outbound peer: not taken; the warning
     is on and the log has its line.
  e. Two nodes: the second is left DEPTH + 2 blocks deep on a branch the first leaves for a longer one. Over an
     automatic outbound connection it stays there and warns; after addnode it moves to the first node's chain.
 w2. A day after the last such headers the warning clears (the periodic check, run with mockscheduler).
 w3. Compressed headers (headers2) of a new deep branch from a new outbound peer turn it on again, with a new line.
 w4. Such headers again within the hour: no new line, the warning stays on; an hour after the last line: a new one.
  i. -testunbackedforkdepth outside 1 to MAX_UNBACKED_FORK_DEPTH stops the start (a full run only).

--case runs one case after the setup it needs, for the negative controls: a build without the exemption fails h2,
c2 and e on their own assertions, one without the warning fails w1 and w3, one that logs every time fails w4.
"""

import time

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import create_coinbase
from test_framework.messages import (CBlock, CBlockHeader, CompressibleBlockHeader, HeaderAndShortIDs, msg_cmpctblock,
                                     msg_headers, msg_headers2, uint256_from_compact)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (FAST_STAKING_ARG, assert_equal, assert_greater_than, force_finish_mnsync, p2p_port,
                                 set_node_times)

# chainparams.cpp, CRegTestParams
LAST_POW_BLOCK = 5000
POS_TARGET_SPACING = 150
POS_LIMIT = (1 << 224) - 1
# pow.cpp, Lwma3CalculateNextWorkRequired
LWMA_N = 36
# net_processing.h, and the depth limit this test runs with (-testunbackedforkdepth)
MAX_UNBACKED_FORK_DEPTH = 100
DEPTH = 10
# net_processing.cpp: DEEP_FORK_WARNING_EXPIRY, how often the periodic check that clears it runs
# (EXTRA_PEER_CHECK_INTERVAL), and DEEP_FORK_LOG_INTERVAL
DAY = 24 * 60 * 60
EXTRA_PEER_CHECK_INTERVAL = 45
HOUR = 60 * 60
# warnings.cpp; the log line without -debug and the -debug=net line (net_processing.cpp)
WARNING = "branches off this node's chain too far below its tip"
LOG_LINE = "serves headers on a branch off this node's chain at height"
DROPPED = "more than %d below the tip, dropped" % DEPTH

STAKE_MIN_AGE = 10 * 60
POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
# as in p2p_unbacked_pos_headers.py: a step of FAST_STEP always opens a new search time for the minter, which runs at
# the fast pace (FAST_STAKING_ARG)
FAST_STEP = 8
STAKE_POLL = 6
SECONDS_PER_BLOCK = 15
# proof-of-stake blocks before the cases: a branch off DEPTH + 1 below the tip still starts above the last
# proof-of-work block, where the limits apply
POS_BLOCKS = DEPTH + 6
CASES = ["h1", "h2", "c1", "c2", "w1", "e", "w2", "w3", "w4"]


def compact_from_uint256(value):
    size = (value.bit_length() + 7) // 8
    if size <= 3:
        compact = (value << (8 * (3 - size))) & 0xffffffff
    else:
        compact = (value >> (8 * (size - 3))) & 0xffffffff
    if compact & 0x00800000:
        compact >>= 8
        size += 1
    return compact | (size << 24)


def next_pos_bits(chain):
    """Port of Lwma3CalculateNextWorkRequired (pow.cpp), as in p2p_unbacked_pos_headers.py."""
    k = LWMA_N * (LWMA_N + 1) * POS_TARGET_SPACING // 2
    previous_time = chain[0][0]
    weighted, sum_target = 0, 0
    for j, (block_time, bits) in enumerate(chain[1:], start=1):
        this_time = block_time if block_time > previous_time else previous_time + 1
        weighted += min(6 * POS_TARGET_SPACING, this_time - previous_time) * j
        previous_time = this_time
        sum_target += uint256_from_compact(bits) // (k * LWMA_N)
    return compact_from_uint256(min(weighted * sum_target, POS_LIMIT))


class UnbackedDeepForkTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--case", choices=CASES, default=None, help="run one case, after the setup it needs")

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-staking=1", "-debug=net", FAST_STAKING_ARG, "-testunbackedforkdepth=%d" % DEPTH],
                           ["-debug=net", "-testunbackedforkdepth=%d" % DEPTH]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # The first node listens on a second port too, which gives its peers the noban permission. Its own port stays
        # bound by the framework's configuration (bind=127.0.0.1): a -bind for it here bound it twice.
        self.noban_port = p2p_port(self.num_nodes + 2)
        self.extra_args[0] += ["-whitebind=noban@127.0.0.1:%d" % self.noban_port]
        self.setup_nodes()
        self.connect_automatic(1, 0)

    def connect_automatic(self, a, b):
        """Node `a` opens an automatic outbound connection to node `b` (connect_nodes uses addnode, a manual one)."""
        self.nodes[a].addconnection("127.0.0.1:%d" % p2p_port(b), "outbound-full-relay", False)
        self.wait_until(lambda: [p["connection_type"] for p in self.nodes[a].getpeerinfo()] == ["outbound-full-relay"]
                        and all(p["version"] != 0 for p in self.nodes[a].getpeerinfo()))

    def disconnect_all(self, a):
        node = self.nodes[a]
        for peer in node.getpeerinfo():
            node.disconnectnode(nodeid=peer["id"])
        self.wait_until(lambda: len(node.getpeerinfo()) == 0)

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def tail(self, node, block_hash):
        entries = []
        for _ in range(LWMA_N + 1):
            header = node.getblockheader(block_hash)
            entries.append((header["time"], int(header["bits"], 16)))
            block_hash = header["previousblockhash"]
        return entries[::-1]

    def branch(self, node, block_hash, count, tag):
        """`count` headers in a row on `block_hash`, a second apart, no block behind any; `tag` sets them apart."""
        tail = self.tail(node, block_hash)
        prev_hash, prev_time = int(block_hash, 16), tail[-1][0]
        version = node.getblockheader(block_hash)["version"]
        headers = []
        for _ in range(count):
            header = CBlockHeader()
            header.nVersion = version
            header.hashPrevBlock = prev_hash
            header.hashMerkleRoot = tag
            header.nTime = prev_time + 1
            header.nBits = next_pos_bits(tail)
            header.nNonce = 0
            header.rehash()
            headers.append(header)
            tail = (tail + [(header.nTime, header.nBits)])[-(LWMA_N + 1):]
            prev_hash, prev_time = header.sha256, header.nTime
        return headers

    def stake_to(self, node, height, step):
        """As in p2p_unbacked_pos_headers.py: stake up to `height`, moving the clock `step` on each round."""
        deadline = time.time() + (height - node.getblockcount()) * SECONDS_PER_BLOCK * self.options.timeout_factor + 120
        while node.getblockcount() < height:
            assert time.time() < deadline, "no block staked at height %d" % (node.getblockcount() + 1)
            before = node.getblockcount()
            self.advance_clock(step)
            poll_until = time.time() + STAKE_POLL
            while time.time() < poll_until and node.getblockcount() == before:
                time.sleep(0.2)

    def stake_blocks(self, count, first_step=FAST_STEP):
        node = self.nodes[0]
        target = node.getblockcount() + count
        assert_equal(node.setstaking(self.wallet_id), True)  # a toggle: True is "now on"
        self.stake_to(node, node.getblockcount() + 1, first_step)
        self.stake_to(node, target, FAST_STEP)
        assert_equal(node.setstaking(self.wallet_id), False)
        time.sleep(3)  # a round in flight ends
        assert_equal(node.getblockcount(), target)

    def indexed(self, node, block_hash):
        try:
            node.getblockheader(block_hash)
            return True
        except JSONRPCException:
            return False

    def has_block(self, node, block_hash):
        try:
            node.getblock(block_hash, 0)  # fails unless its block is stored
            return True
        except JSONRPCException:
            return False

    def warned(self, node):
        return WARNING in str(node.getnetworkinfo()["warnings"])

    def deep_parent(self):
        """The active chain's block DEPTH + 1 below the tip: a branch on it is too deep to take."""
        node = self.nodes[0]
        height = node.getblockcount() - DEPTH - 1
        assert_greater_than(height, LAST_POW_BLOCK)
        return node.getblockhash(height)

    def ordinary_peer(self):
        return self.nodes[0].add_p2p_connection(P2PInterface())

    def noban_peer(self):
        peer = self.nodes[0].add_p2p_connection(P2PInterface(), dstport=self.noban_port)
        assert "noban" in self.nodes[0].getpeerinfo()[-1]["permissions"]
        return peer

    def leave(self, peer):
        peer.peer_disconnect()
        peer.wait_for_disconnect()

    def compact_block_on(self, node, parent, tag):
        """A compact block on `parent` (an index entry), with its header."""
        header = self.branch(node, parent.hash, 1, tag=tag)[0]
        block = CBlock(header)
        block.vtx = [create_coinbase(node.getblockheader(parent.hash)["height"] + 1)]
        for tx in block.vtx:
            tx.rehash()
        cmpct = HeaderAndShortIDs()
        cmpct.initialize_from_block(block)
        assert_equal(cmpct.header.hash, header.hash)
        return header, cmpct

    def run_test(self):
        node, other = self.nodes
        only = self.options.case
        self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
        set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining to the proof-of-stake boundary (height %d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount()), address,
                                   sync_fun=lambda: self.sync_blocks([node, other]))
        force_finish_mnsync(node)
        assert node.getblockheader(node.getbestblockhash())["time"] >= self.mocktime
        self.wallet_id = int(next(iter(node.liststakingwallets())))
        self.log.info("Staking %d blocks", POS_BLOCKS)
        self.stake_blocks(POS_BLOCKS, first_step=STAKE_MIN_AGE)
        self.sync_blocks()

        def run(case):
            return only is None or only == case

        if run("h1"):
            self.case_h1()
        if run("h2"):
            self.case_h2()
        if run("c1") or run("c2"):
            self.case_c(run("c1"), run("c2"))
        if run("w1") or only == "w2":
            self.case_w1()  # w2 clears the warning w1 turns on
        if run("e"):
            self.case_e()
        if run("w2"):
            self.case_w2()
        if run("w3") or only == "w4":
            self.case_w3()  # w4 follows the line w3 logs
        if run("w4"):
            self.case_w4()
        if only is None:
            self.log.info("The depth is a test facility from 1 to MAX_UNBACKED_FORK_DEPTH: anything else stops the start")
            self.stop_node(1)
            for value in (0, MAX_UNBACKED_FORK_DEPTH + 1):
                other.assert_start_raises_init_error(["-testunbackedforkdepth=%d" % value],
                                                     "-testunbackedforkdepth must be from 1 to %d" % MAX_UNBACKED_FORK_DEPTH,
                                                     match=ErrorMatch.PARTIAL_REGEX)

    def case_h1(self):
        node = self.nodes[0]
        self.log.info("h1. A branch offering more work, off %d blocks below the tip, from an ordinary inbound peer",
                      DEPTH + 1)
        # As long as w1's, which warns from an outbound peer: here only the direction keeps the warning off.
        branch = self.branch(node, self.deep_parent(), DEPTH + 6, tag=0x110)
        peer = self.ordinary_peer()
        with node.assert_debug_log(expected_msgs=[DROPPED], unexpected_msgs=[LOG_LINE]):
            peer.send_and_ping(msg_headers(branch))
        taken = self.indexed(node, branch[0].hash)
        self.log.info("taken: %s, the node warns: %s, connected: %s", taken, self.warned(node), peer.is_connected)
        assert not taken
        assert not self.warned(node)
        assert peer.is_connected
        self.leave(peer)

    def case_h2(self):
        node = self.nodes[0]
        self.log.info("h2. A short branch off the same block from a noban peer")
        branch = self.branch(node, self.deep_parent(), 3, tag=0x120)
        best = node.getbestblockhash()
        peer = self.noban_peer()
        peer.send_and_ping(msg_headers(branch))
        status = {t["hash"]: t["status"] for t in node.getchaintips()}.get(branch[-1].hash)
        self.log.info("taken: %s (%s), the active chain unchanged: %s", status is not None, status,
                      node.getbestblockhash() == best)
        assert_equal(status, "headers-only")
        assert_equal(node.getbestblockhash(), best)
        self.leave(peer)

    def case_c(self, check_c1, check_c2):
        node = self.nodes[0]
        height = node.getblockcount()
        self.log.info("c. A branch's first header at height %d, taken while it branches off 2 blocks below the tip; "
                      "then %d blocks on the active chain", height - 1, DEPTH + 1)
        first = self.branch(node, node.getblockhash(height - 2), 1, tag=0x130)[0]  # less work than the tip
        peer = self.ordinary_peer()
        peer.send_and_ping(msg_headers([first]))
        assert self.indexed(node, first.hash)  # shallow: taken from anyone, on any build
        self.leave(peer)
        self.stake_blocks(DEPTH + 1)
        self.sync_blocks()
        header, cmpct = self.compact_block_on(node, first, tag=0x131)
        if check_c1:
            self.log.info("c1. A compact block on it (off %d blocks below the tip now) from an ordinary peer", DEPTH + 3)
            peer = self.ordinary_peer()
            with node.assert_debug_log(expected_msgs=["cmpctblock %s at height %d" % (header.hash, height)]):
                peer.send_and_ping(msg_cmpctblock(cmpct.to_p2p()))
            taken = self.indexed(node, header.hash)
            self.log.info("its header taken: %s, connected: %s", taken, peer.is_connected)
            assert not taken
            assert peer.is_connected
            self.leave(peer)
        if check_c2:
            self.log.info("c2. The same compact block from a noban peer")
            best = node.getbestblockhash()
            peer = self.noban_peer()
            peer.send_and_ping(msg_cmpctblock(cmpct.to_p2p()))
            taken, stored = self.indexed(node, header.hash), self.has_block(node, header.hash)
            self.log.info("its header taken: %s, its block stored: %s, the active chain unchanged: %s", taken, stored,
                          node.getbestblockhash() == best)
            assert taken
            assert not stored
            assert_equal(node.getbestblockhash(), best)
            self.leave(peer)

    def case_w1(self):
        node = self.nodes[0]
        self.log.info("w1. A branch offering more work, off %d blocks below the tip, from an outbound peer", DEPTH + 1)
        branch = self.branch(node, self.deep_parent(), DEPTH + 6, tag=0x150)
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=0)
        with node.assert_debug_log(expected_msgs=[DROPPED, LOG_LINE]):
            peer.send_and_ping(msg_headers(branch))
        taken = self.indexed(node, branch[0].hash)
        self.log.info("taken: %s, the node warns: %s", taken, self.warned(node))
        assert not taken
        assert self.warned(node)
        self.leave(peer)

    def case_e(self):
        node, other = self.nodes
        self.log.info("e. The second node on a branch %d blocks deep that the first leaves for a longer one", DEPTH + 2)
        self.sync_blocks()
        fork = node.getbestblockhash()
        self.stake_blocks(DEPTH + 2)
        self.sync_blocks()
        left = other.getbestblockhash()
        first_left = node.getblockhash(node.getblockheader(fork)["height"] + 1)
        self.disconnect_all(1)
        node.invalidateblock(first_left)
        assert_equal(node.getbestblockhash(), fork)
        self.stake_blocks(DEPTH + 5)
        # A valid branch with less work again (the second node's headers on it must not count as invalid here).
        node.reconsiderblock(first_left)
        network = node.getbestblockhash()
        assert network != left
        self.log.info("over an automatic outbound connection the second node stays on its branch and warns")
        with other.assert_debug_log(expected_msgs=[DROPPED, LOG_LINE], timeout=60):
            self.connect_automatic(1, 0)
        self.log.info("the second node is still on its branch: %s, it warns: %s", other.getbestblockhash() == left,
                      self.warned(other))
        assert_equal(other.getbestblockhash(), left)
        assert self.warned(other)
        self.disconnect_all(1)
        self.log.info("after addnode it moves to the first node's chain")
        other.addnode("127.0.0.1:%d" % p2p_port(0), "onetry")
        self.wait_until(lambda: other.getbestblockhash() == network, timeout=60)
        self.log.info("the second node is on the first node's chain at %d", other.getblockcount())
        self.disconnect_all(1)

    def case_w2(self):
        node = self.nodes[0]
        self.log.info("w2. A day after the last such headers the warning clears")
        assert self.warned(node), "precondition: the warning is on (w1)"
        node.disconnect_p2ps()
        if self.nodes[1].getpeerinfo():
            self.disconnect_all(1)
        self.mocktime += DAY + 1
        node.setmocktime(self.mocktime)

        def cleared():
            node.mockscheduler(EXTRA_PEER_CHECK_INTERVAL)
            return not self.warned(node)
        self.wait_until(cleared, timeout=30)
        self.log.info("the warning cleared")

    def case_w3(self):
        node = self.nodes[0]
        self.log.info("w3. Compressed headers of a new deep branch, offering more work, from a new outbound peer")
        assert not self.warned(node), "precondition: the warning is off"
        branch = self.branch(node, self.deep_parent(), DEPTH + 6, tag=0x170)
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=1)
        with node.assert_debug_log(expected_msgs=[DROPPED, LOG_LINE]):
            peer.send_and_ping(msg_headers2([CompressibleBlockHeader(h) for h in branch]))
        taken = self.indexed(node, branch[0].hash)
        self.log.info("taken: %s, the node warns: %s", taken, self.warned(node))
        assert not taken
        assert self.warned(node)
        self.leave(peer)

    def case_w4(self):
        node = self.nodes[0]
        self.log.info("w4. Such headers again within the hour: no new line; an hour after the last line: a new one")
        assert self.warned(node), "precondition: the warning is on and its line was logged just now (w3)"
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=2)
        with node.assert_debug_log(expected_msgs=[DROPPED], unexpected_msgs=[LOG_LINE]):
            peer.send_and_ping(msg_headers(self.branch(node, self.deep_parent(), DEPTH + 6, tag=0x180)))
        assert self.warned(node)
        self.leave(peer)
        self.mocktime += HOUR
        node.setmocktime(self.mocktime)
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=3)
        with node.assert_debug_log(expected_msgs=[DROPPED, LOG_LINE]):
            peer.send_and_ping(msg_headers(self.branch(node, self.deep_parent(), DEPTH + 6, tag=0x190)))
        assert self.warned(node)
        self.leave(peer)
        self.log.info("one line an hour, the warning on throughout")


if __name__ == '__main__':
    UnbackedDeepForkTest().main()
