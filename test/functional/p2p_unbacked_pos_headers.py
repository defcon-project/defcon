#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Headers without their blocks above the last proof-of-work block: what a node takes.

A proof-of-stake header carries no kernel and no block signature, so a peer can make any number of them for
nothing, and each new one stays in the block index (and on disk). The node takes a header without its block only
up to UNBACKED_HEADER_AHEAD heights above its active tip (or the last proof-of-work block), plus
UNBACKED_HEADER_AHEAD_PER_SPACING per block interval the tip's time lies behind, asking for the rest again later;
not at all if it branches off the active chain more than MAX_UNBACKED_FORK_DEPTH blocks below the tip; of the new
branches a peer opens (a header without its block, or a block it was not asked for, on a block that already has a
child) at most MAX_UNBACKED_FORKS_PER_PEER may be on a block never connected (that a block is stored is not
enough: one never connected has not had its stake checked), counted over a day (a rate, not a bound for life), and
past that its further new branches are not taken -- nothing else happens to the peer: a real block that loses here
is, to this node, the same as one made for nothing; for a day after the limits (or validation, for an unrequested
block) dropped anything from a peer, its headers that do not connect here are not counted as such, whatever they
build on (steps 2, 3, 5, 6, 9, 9b and 10 send MAX_UNCONNECTING_HEADERS times as many in a row; step 11 checks that
the count is back a day later); below the minimum chain work the height limit does not apply; and an unrequested
block it would drop anyway leaves no index entry.

Each step logs its numbers before it checks them, so a run on a node without the limits measures it as well.

 0. A node following the chain while a peer puts a header without its block on each new tip before the real
    block is announced: every real block opens a new branch, more than MAX_UNBACKED_FORKS_PER_PEER of them, and
    the peer serving them is not punished (their blocks come).
 1. A node that fell behind (AWAY_BLOCKS blocks, staked as fast as the clock allows) while a peer handed it a
    branch that is now its best header: the real chain comes in cut at the height limit, the rest is asked for
    again, and the peer serving it is not punished (it opens one branch).
 2. A 200-header branch above the tip, in one message: taken up to the height limit.
 3. Single sibling headers: from the 33rd new branch on none is taken, and the peer stays. It then announces on
    a branch not taken, more often than MAX_UNCONNECTING_HEADERS would punish: (a) one announcement on the other,
    with no reply in between, and (b) after another branch was dropped, back on the first one: not punished.
    (c) A peer sending headers that do not connect, on nothing dropped from it, still is.
 4. The same with an invalid (too far in the future) header after each sibling: still counted.
 5. Siblings branching off 101 blocks below the tip: not taken, no punishment.
 6. An unrequested block on a lower-work branch, signed with the sender's own key: no index entry; the sender's
    announcements on it are not punished.
 7. The limit grows with the tip's age: the 200-header branch again, 450 s later, is taken 12 heights further.
 8. A third node syncs from scratch.
 9. Copies of the tip with the same work, signed with the sender's own key, header first: their blocks are stored
    but never connected, so their branches still count; the 33rd is not taken (as a header, or as a compact
    block), and the peer stays. To this node
    these are what an honest peer announcing real blocks that lose here would send: such blocks are fetched and
    stored the same way, and their stake is not checked either, as they are never connected.
9b. The same copies block first, not asked for: each opens a branch that counts as well (else headers on them
    would open branches for nothing); the 33rd block is not taken, and the peer stays.
10. A reply cut at the height limit is asked on from where it was cut, for a peer with no best known block, one
    whose best known block is on another branch, and after a compact block above the limit (that peer's only drop:
    its announcements on it are not punished).
11. The branch count is a rate: a day later, on the same connection, the peer may open as many again, and one
    more within that window is not taken; and a day after that drop its headers that do not connect count again.
12. Below the minimum chain work the height limit does not apply: a node behind more blocks than it allows syncs
    without the clock moving.
"""

import time

from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.messages import (CBlock, CBlockHeader, HeaderAndShortIDs, from_hex, msg_block, msg_cmpctblock,
                                     msg_headers, ser_uint256, uint256_from_compact)
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.script import CScript, OP_CHECKSIG
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error, force_finish_mnsync, set_node_times

# chainparams.cpp, CRegTestParams
LAST_POW_BLOCK = 5000
POS_TARGET_SPACING = 150
POS_LIMIT = (1 << 224) - 1
# chain.h
MAX_FUTURE_BLOCK_TIME = 3 * 60
# net_processing.cpp
UNBACKED_HEADER_AHEAD = 24
UNBACKED_HEADER_AHEAD_PER_SPACING = 4
MAX_UNBACKED_FORK_DEPTH = 100
MAX_UNBACKED_FORKS_PER_PEER = 32
UNBACKED_FORK_WINDOW = 24 * 60 * 60
MAX_UNCONNECTING_HEADERS = 10
# net_processing.cpp, HandleFewUnconnectingHeaders: 20 points every MAX_UNCONNECTING_HEADERS in a row; 100 disconnect
UNCONNECTING_ROUNDS = 5 * MAX_UNCONNECTING_HEADERS
# new branches sent past the count, none of which may be taken
EXTRA_BRANCHES = 8
# pow.cpp, Lwma3CalculateNextWorkRequired
LWMA_N = 36

STAKE_MIN_AGE = 10 * 60
POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
# pos/minter.cpp: the minter tries one search time per round, and a round takes 2.5 s (5 s after a block); the
# search time is the clock with posTimestampMask (5) cleared, so a step of 8 s always opens a new one.
FAST_STEP = 8
STAKE_POLL = 6
SECONDS_PER_BLOCK = 15
# The tip ends at least MAX_UNBACKED_FORK_DEPTH + 1 above a proof-of-stake block (step 5); the second node misses
# the last AWAY_BLOCKS of them, more than the height limit lets it take at once (step 1).
TIP = LAST_POW_BLOCK + 1 + MAX_UNBACKED_FORK_DEPTH + 1
AWAY_BLOCKS = 48
LONG_BRANCH = 200
LATER = 3 * POS_TARGET_SPACING
# step 11: the clock moves a day in hops shorter than the node's inactivity timeout (20 minutes)
CLOCK_HOP = 10 * 60
# step 12: more blocks than the height limit lets a node take at once while they come FAST_STEP apart. The limit
# grows UNBACKED_HEADER_AHEAD_PER_SPACING heights per POS_TARGET_SPACING of the tip's age, so 48 blocks staked 8 s
# apart end about 16 heights above it; the step asserts that margin before it relies on it.
MIN_WORK_BLOCKS = 48


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
    """Port of Lwma3CalculateNextWorkRequired (pow.cpp), as in feature_pos_header_only.py."""
    k = LWMA_N * (LWMA_N + 1) * POS_TARGET_SPACING // 2
    previous_time = chain[0][0]
    weighted, sum_target = 0, 0
    for j, (block_time, bits) in enumerate(chain[1:], start=1):
        this_time = block_time if block_time > previous_time else previous_time + 1
        weighted += min(6 * POS_TARGET_SPACING, this_time - previous_time) * j
        previous_time = this_time
        sum_target += uint256_from_compact(bits) // (k * LWMA_N)
    return compact_from_uint256(min(weighted * sum_target, POS_LIMIT))


class UnbackedPosHeadersTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [["-staking=1", "-debug=net"], ["-debug=net"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(1, 0)  # the third node stays apart until step 8

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

    def branch(self, node, block_hash, count, tag, last_time=None):
        """`count` headers in a row on `block_hash`, a second apart, no block behind any; `tag` sets them apart
        from any other branch on the same block; `last_time` overrides the last header's time."""
        tail = self.tail(node, block_hash)
        prev_hash, prev_time = int(block_hash, 16), tail[-1][0]
        version = node.getblockheader(block_hash)["version"]
        headers = []
        for i in range(count):
            header = CBlockHeader()
            header.nVersion = version
            header.hashPrevBlock = prev_hash
            header.hashMerkleRoot = tag
            header.nTime = last_time if (last_time is not None and i == count - 1) else prev_time + 1
            header.nBits = next_pos_bits(tail)
            header.nNonce = 0
            header.rehash()
            headers.append(header)
            tail = (tail + [(header.nTime, header.nBits)])[-(LWMA_N + 1):]
            prev_hash, prev_time = header.sha256, header.nTime
        return headers

    def limit(self, node):
        tip = node.getblockheader(node.getbestblockhash())
        behind = max(0, self.mocktime - tip["time"])
        return max(tip["height"], LAST_POW_BLOCK) + UNBACKED_HEADER_AHEAD + UNBACKED_HEADER_AHEAD_PER_SPACING * (behind // POS_TARGET_SPACING)

    def entries(self, node):
        """Block index entries: the active chain and every other branch, counted from getchaintips."""
        return node.getblockcount() + sum(t["branchlen"] for t in node.getchaintips() if t["status"] != "active")

    def stake_to(self, node, height, step):
        """Stake up to `height`, moving the clock `step` on and waiting for the minter's next round each time;
        returns how many steps it took."""
        start = node.getblockcount()
        deadline = time.time() + (height - start) * SECONDS_PER_BLOCK * self.options.timeout_factor + 120
        steps = 0
        while node.getblockcount() < height:
            assert time.time() < deadline, "no block staked at height %d" % (node.getblockcount() + 1)
            before = node.getblockcount()
            self.advance_clock(step)
            steps += 1
            poll_until = time.time() + STAKE_POLL
            while time.time() < poll_until and node.getblockcount() == before:
                time.sleep(0.2)
        return steps

    def until_synced(self, a, b, steps=90):
        for _ in range(int(steps * self.options.timeout_factor)):
            if a.getbestblockhash() == b.getbestblockhash():
                return
            # past HEADERS_RESPONSE_TIME in a few rounds, for a request after a cut; not while `a` waits for a block
            # (a block in flight for one block interval of clock times out and `a` drops the peer serving it)
            if not any(p["inflight"] for p in a.getpeerinfo()):
                self.advance_clock(30)
            time.sleep(2)
        raise AssertionError("not synced: %d vs %d" % (a.getblockcount(), b.getblockcount()))

    def copy_signed_by(self, node, block_hash, key, tag):
        """The staked block at `block_hash`, its coinstake paying `key` and signed by it, the coinbase tagged."""
        block = from_hex(CBlock(), node.getblock(block_hash, 0))
        assert block.is_proof_of_stake()
        coinbase = block.vtx[0]
        coinbase.vin[0].scriptSig = CScript(bytes(coinbase.vin[0].scriptSig) + bytes(CScript([tag])))
        coinbase.rehash()
        block.vtx[1].vout[1].scriptPubKey = CScript([key.get_pubkey().get_bytes(), OP_CHECKSIG])
        block.vtx[1].rehash()
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.vchBlockSig = key.sign_ecdsa(ser_uint256(block.sha256))
        return block

    def announce_on_dropped(self, node, peer, dropped, tag, what):
        """UNCONNECTING_ROUNDS announcements in a row from `peer` on `dropped` (a header, compact block or block the
        limits, or validation, dropped from it), with no reply in between: none can connect here, which must not add
        up to a punishment. Not validated (they do not connect), so only their links matter."""
        tip = node.getblockheader(node.getbestblockhash())
        chain, prev_hash, prev_time = [], dropped.sha256, dropped.nTime
        for _ in range(UNCONNECTING_ROUNDS):
            header = CBlockHeader()
            header.nVersion, header.hashPrevBlock, header.hashMerkleRoot = tip["version"], prev_hash, tag
            header.nTime, header.nBits, header.nNonce = prev_time + 1, int(tip["bits"], 16), 0
            header.rehash()
            chain.append(header)
            prev_hash, prev_time = header.sha256, header.nTime
        for header in chain:
            peer.send_and_ping(msg_headers([header]))
        tips = [t["hash"] for t in node.getchaintips()]
        self.log.info("%s: %d announcements in a row on what was dropped: none taken %s, connected=%s",
                      what, UNCONNECTING_ROUNDS, not any(h.hash in tips for h in chain), peer.is_connected)
        assert not any(h.hash in tips for h in chain)
        assert peer.is_connected

    def has_block(self, node, block_hash):
        try:
            node.getblock(block_hash, 0)  # fails unless its block is stored
            return True
        except JSONRPCException:
            return False

    def flood_siblings(self, node, parent, tag, invalid_child=False):
        """Single sibling headers on `parent` from one fresh peer, MAX_UNBACKED_FORKS_PER_PEER and then EXTRA_BRANCHES
        more; returns how many index entries they left, and the peer (connected)."""
        before = self.entries(node)
        peer = node.add_p2p_connection(P2PInterface())
        for i in range(MAX_UNBACKED_FORKS_PER_PEER + EXTRA_BRANCHES):
            headers = self.branch(node, parent, 1, tag=tag + i)
            if invalid_child:
                headers.append(self.branch(node, parent, 2, tag=tag + i, last_time=self.mocktime + MAX_FUTURE_BLOCK_TIME + 600)[1])
            peer.send_and_ping(msg_headers(headers))
            if i == MAX_UNBACKED_FORKS_PER_PEER - 1:
                self.log.info("after %d new branches: index entries added %d", MAX_UNBACKED_FORKS_PER_PEER, self.entries(node) - before)
        grown = self.entries(node) - before
        self.log.info("after %d more: index entries added %d in all, connected=%s", EXTRA_BRANCHES, grown, peer.is_connected)
        assert peer.is_connected
        return grown, peer

    def run_test(self):
        node, behind_node, fresh = self.nodes
        self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
        set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining to the proof-of-stake boundary (height %d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            # the second node holds every block before the clock moves on: one still being downloaded when the
            # clock jumps (here or by STAKE_MIN_AGE below) times out and the node drops the peer serving it
            self.generatetoaddress(node, min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount()), address,
                                   sync_fun=lambda: self.sync_blocks([node, behind_node]))
        force_finish_mnsync(node)
        self.advance_clock(STAKE_MIN_AGE)
        wallet_id = int(next(iter(node.liststakingwallets())))
        assert_equal(node.setstaking(wallet_id), True)  # a toggle: True is "now on"
        away_from = TIP - AWAY_BLOCKS
        self.log.info("0. Staking to %d a block interval apart; before each block a peer puts a header without its "
                      "block on the second node's tip, so the real block opens a new branch there", away_from)
        steps = 0
        while node.getblockcount() < away_from:
            self.sync_blocks([node, behind_node])
            front = behind_node.add_p2p_connection(P2PInterface())
            front.send_and_ping(msg_headers(self.branch(behind_node, behind_node.getbestblockhash(), 1, tag=0x500 + node.getblockcount())))
            front.peer_disconnect()
            front.wait_for_disconnect()
            steps += self.stake_to(node, node.getblockcount() + 1, POS_TARGET_SPACING)
        self.sync_blocks([node, behind_node])
        served = [p for p in behind_node.getpeerinfo() if not p["inbound"]]
        self.log.info("%d blocks in %d steps, each after a header put on the tip; the peer serving them is still "
                      "connected: %s", away_from - LAST_POW_BLOCK, steps, len(served) == 1)
        assert_greater_than(away_from - LAST_POW_BLOCK, MAX_UNBACKED_FORKS_PER_PEER)
        assert_equal(len(served), 1)
        self.disconnect_nodes(1, 0)

        self.log.info("1. %d blocks staked %d s apart while the second node is away", AWAY_BLOCKS, FAST_STEP)
        behind_tip = behind_node.getbestblockhash()
        steps = self.stake_to(node, TIP, FAST_STEP)
        assert_equal(node.setstaking(wallet_id), False)
        time.sleep(3)
        assert_equal(node.getblockcount(), TIP)
        limit = self.limit(behind_node)
        self.log.info("%d blocks in %d steps; the second node is at %d, the chain at %d; it takes headers without "
                      "blocks up to %d", AWAY_BLOCKS, steps, behind_node.getblockcount(), TIP, limit)
        assert_greater_than(TIP, limit)  # the real chain comes in cut
        fake = behind_node.add_p2p_connection(P2PInterface())
        fake.send_and_ping(msg_headers(self.branch(behind_node, behind_tip, limit - behind_node.getblockcount(), tag=0xf)))
        self.log.info("a peer's branch on the second node: its best header is now at %d (the real chain is cut at it)",
                      behind_node.getblockchaininfo()["headers"])
        assert_equal(behind_node.getblockchaininfo()["headers"], limit)
        fake.peer_disconnect()
        fake.wait_for_disconnect()
        self.connect_nodes(1, 0)
        self.until_synced(behind_node, node)
        outbound = [p for p in behind_node.getpeerinfo() if not p["inbound"]]
        self.log.info("the second node synced to %d; its peer serving the chain is still connected: %s",
                      behind_node.getblockcount(), len(outbound) == 1)
        assert_equal(len(outbound), 1)
        self.disconnect_nodes(1, 0)

        tip_hash = node.getbestblockhash()
        tip = node.getblockheader(tip_hash)
        base_entries = self.entries(node)

        self.log.info("2. A %d-header branch above the tip, in one message", LONG_BRANCH)
        long_branch = self.branch(node, tip_hash, LONG_BRANCH, tag=0xa)
        limit_now = self.limit(node)
        long_peer = node.add_p2p_connection(P2PInterface())
        long_peer.send_and_ping(msg_headers(long_branch))
        headers = node.getblockchaininfo()["headers"]
        self.log.info("headers taken: up to height %d of %d sent (limit %d, tip %d)", headers, tip["height"] + LONG_BRANCH, limit_now, tip["height"])
        assert_equal(headers, limit_now)
        assert long_peer.is_connected
        self.announce_on_dropped(node, long_peer, long_branch[-1], tag=0xa1, what="cut at the height limit")

        self.log.info("3. Single sibling headers on the tip, from one peer")
        grown, flood_peer = self.flood_siblings(node, tip_hash, tag=0x1000)
        assert_equal(grown, MAX_UNBACKED_FORKS_PER_PEER)
        # The first branch not taken and a chain of announcements on it (none of which connects here), and another
        # branch not taken.
        chain = self.branch(node, tip_hash, UNCONNECTING_ROUNDS + 1, tag=0x1000 + MAX_UNBACKED_FORKS_PER_PEER)
        not_taken, other = chain[0], self.branch(node, tip_hash, 1, tag=0x1000 + MAX_UNBACKED_FORKS_PER_PEER + 1)[0]
        def none_taken(headers):
            tips = [t["hash"] for t in node.getchaintips()]
            return not any(h.hash in tips for h in headers)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, not_taken.hash)
        # (a) The branch again, then announcements on it one after another, before any reply (each would bring a
        # request): more than MAX_UNCONNECTING_HEADERS would punish.
        flood_peer.send_and_ping(msg_headers([not_taken]))
        for header in chain[1:]:
            flood_peer.send_and_ping(msg_headers([header]))
        self.log.info("%d announcements in a row on the branch not taken: none taken %s, connected=%s",
                      UNCONNECTING_ROUNDS, none_taken(chain), flood_peer.is_connected)
        assert none_taken(chain)
        assert flood_peer.is_connected
        # (b) Another branch dropped after it, then the peer goes back to the first: its announcements there, one after
        # another before any reply, do not count either, whichever branch was dropped last.
        flood_peer.send_and_ping(msg_headers([other]))
        assert none_taken([other])
        self.announce_on_dropped(node, flood_peer, chain[-1], tag=0x1e00, what="back on the first branch after another was dropped")
        # (c) A peer that sends headers that do not connect, on nothing dropped from it: punished as before.
        broken = node.add_p2p_connection(P2PInterface())
        unknown = self.branch(node, tip_hash, UNCONNECTING_ROUNDS + 1, tag=0x1f00)
        for header in unknown[1:]:
            broken.send_message(msg_headers([header]))
        broken.wait_for_disconnect()
        self.log.info("a peer sending %d headers that do not connect, on nothing dropped from it: disconnected", UNCONNECTING_ROUNDS)

        self.log.info("4. The same, each followed by a header too far in the future")
        grown, _ = self.flood_siblings(node, tip_hash, tag=0x2000, invalid_child=True)
        assert_equal(grown, MAX_UNBACKED_FORKS_PER_PEER)

        self.log.info("5. Siblings branching off %d blocks below the tip", MAX_UNBACKED_FORK_DEPTH + 1)
        deep_height = tip["height"] - MAX_UNBACKED_FORK_DEPTH - 1
        assert_greater_than(deep_height, LAST_POW_BLOCK)
        deep_parent = node.getblockhash(deep_height)
        before = self.entries(node)
        deep_peer = node.add_p2p_connection(P2PInterface())
        for i in range(MAX_UNBACKED_FORKS_PER_PEER + 8):
            deep_branch = self.branch(node, deep_parent, 3, tag=0x3000 + i)
            deep_peer.send_and_ping(msg_headers(deep_branch))
        self.log.info("after %d deep branches: index entries added %d, connected=%s",
                      MAX_UNBACKED_FORKS_PER_PEER + 8, self.entries(node) - before, deep_peer.is_connected)
        assert_equal(self.entries(node), before)
        assert deep_peer.is_connected
        self.announce_on_dropped(node, deep_peer, deep_branch[-1], tag=0x3fff, what="too deep")

        self.log.info("6. An unrequested block on a lower-work branch, signed with the sender's own key")
        key = ECKey()
        key.generate()
        forged = self.copy_signed_by(node, node.getblockhash(tip["height"] - 1), key, tag=0x6)
        block_peer = node.add_p2p_connection(P2PInterface())
        block_peer.send_and_ping(msg_block(forged))
        known = forged.hash in [t["hash"] for t in node.getchaintips()]
        self.log.info("block %s at height %d sent; an index entry for it: %s", forged.hash, tip["height"] - 1, known)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, forged.hash)
        assert block_peer.is_connected
        # The block was this peer's only drop, by validation: its announcements on it do not count either.
        self.announce_on_dropped(node, block_peer, forged, tag=0x6f, what="dropped by validation, not asked for, less work")

        self.log.info("7. %d s later the limit is %d heights higher (a fresh peer: the node drops one that does not "
                      "send the blocks it asked for)", LATER, LATER // POS_TARGET_SPACING * UNBACKED_HEADER_AHEAD_PER_SPACING)
        self.advance_clock(LATER)
        limit_later = self.limit(node)
        later_peer = node.add_p2p_connection(P2PInterface())
        later_peer.send_and_ping(msg_headers(long_branch))
        headers = node.getblockchaininfo()["headers"]
        self.log.info("headers taken: up to height %d (limit %d)", headers, limit_later)
        assert_equal(headers, limit_later)
        assert_equal(limit_later, limit_now + LATER // POS_TARGET_SPACING * UNBACKED_HEADER_AHEAD_PER_SPACING)
        self.log.info("index entries since the attacks began: %d", self.entries(node) - base_entries)

        self.log.info("8. A third node syncs from scratch")
        self.connect_nodes(2, 0)
        self.until_synced(fresh, node)

        self.log.info("9. Copies of the tip with the same work, signed with the sender's own key: each opens a "
                      "branch and its block comes, but it is never connected (its stake never checked); an honest "
                      "peer's real blocks that lose here come the same way")
        copy_peer = node.add_p2p_connection(P2PInterface())
        stored = 0
        for i in range(MAX_UNBACKED_FORKS_PER_PEER):
            copy = self.copy_signed_by(node, tip_hash, key, tag=0x9000 + i)
            copy_peer.send_and_ping(msg_headers([CBlockHeader(copy)]))
            copy_peer.send_and_ping(msg_block(copy))
            stored += self.has_block(node, copy.hash)
        self.log.info("%d copies sent, header first; blocks stored: %d; the tip is still %s; connected=%s",
                      MAX_UNBACKED_FORKS_PER_PEER, stored, node.getbestblockhash() == tip_hash, copy_peer.is_connected)
        assert_equal(stored, MAX_UNBACKED_FORKS_PER_PEER)
        assert_equal(node.getbestblockhash(), tip_hash)
        assert copy_peer.is_connected
        last = self.copy_signed_by(node, tip_hash, key, tag=0x9000 + MAX_UNBACKED_FORKS_PER_PEER)
        copy_peer.send_and_ping(msg_headers([CBlockHeader(last)]))
        self.log.info("the next copy's header: taken %s, connected=%s",
                      last.hash in [t["hash"] for t in node.getchaintips()], copy_peer.is_connected)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, last.hash)
        assert copy_peer.is_connected
        via_cmpct = self.copy_signed_by(node, tip_hash, key, tag=0x9000 + MAX_UNBACKED_FORKS_PER_PEER + 1)
        for tx in via_cmpct.vtx:
            tx.rehash()  # the short ids are made from the transaction hashes
        cmpct = HeaderAndShortIDs()
        cmpct.initialize_from_block(via_cmpct)
        copy_peer.send_and_ping(msg_cmpctblock(cmpct.to_p2p()))
        self.log.info("one more as a compact block: taken %s, connected=%s",
                      via_cmpct.hash in [t["hash"] for t in node.getchaintips()], copy_peer.is_connected)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, via_cmpct.hash)
        assert copy_peer.is_connected
        self.announce_on_dropped(node, copy_peer, via_cmpct, tag=0x9fff, what="a compact block past the count")

        self.log.info("9b. The same copies block first, not asked for: stored, never connected, each a new branch")
        block_first = node.add_p2p_connection(P2PInterface())
        stored = 0
        for i in range(MAX_UNBACKED_FORKS_PER_PEER):
            copy = self.copy_signed_by(node, tip_hash, key, tag=0x9800 + i)
            block_first.send_and_ping(msg_block(copy))
            stored += self.has_block(node, copy.hash)
        self.log.info("%d copies sent, block first; blocks stored: %d; the tip is still %s; connected=%s",
                      MAX_UNBACKED_FORKS_PER_PEER, stored, node.getbestblockhash() == tip_hash, block_first.is_connected)
        assert_equal(stored, MAX_UNBACKED_FORKS_PER_PEER)
        assert_equal(node.getbestblockhash(), tip_hash)
        last = self.copy_signed_by(node, tip_hash, key, tag=0x9800 + MAX_UNBACKED_FORKS_PER_PEER)
        block_first.send_and_ping(msg_block(last))
        self.log.info("the next copy's block: stored %s, an index entry for it %s, connected=%s", self.has_block(node, last.hash),
                      last.hash in [t["hash"] for t in node.getchaintips()], block_first.is_connected)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, last.hash)
        assert block_first.is_connected
        self.announce_on_dropped(node, block_first, last, tag=0x98ff, what="a block not asked for past the count")

        self.log.info("10. Headers above the limit, cut: each peer is asked again from where its reply was cut, not "
                      "from its best known block (none, or another branch), also after a compact block")
        limit_now = self.limit(node)
        room = limit_now - tip["height"]
        cut_branch = self.branch(node, tip_hash, room + 3, tag=0xc)
        cut_point = cut_branch[room - 1]
        first = node.add_p2p_connection(P2PInterface())
        first.send_and_ping(msg_headers(cut_branch))
        assert_equal(node.getblockheader(cut_point.hash)["height"], limit_now)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, cut_branch[room].hash)
        no_best = node.add_p2p_connection(P2PInterface())
        no_best.send_and_ping(msg_headers([cut_branch[room]]))
        other_best = node.add_p2p_connection(P2PInterface())
        other_best.send_and_ping(msg_headers(self.branch(node, tip_hash, 2, tag=0xd)))
        other_best.send_and_ping(msg_headers([cut_branch[room]]))
        cmpct_block = from_hex(CBlock(), node.getblock(tip_hash, 0))
        above = cut_branch[room]
        cmpct_block.nVersion, cmpct_block.hashPrevBlock, cmpct_block.hashMerkleRoot = above.nVersion, above.hashPrevBlock, above.hashMerkleRoot
        cmpct_block.nTime, cmpct_block.nBits, cmpct_block.nNonce = above.nTime, above.nBits, above.nNonce
        cmpct_block.rehash()
        assert_equal(cmpct_block.sha256, above.sha256)
        for tx in cmpct_block.vtx:
            tx.rehash()  # the short ids are made from the transaction hashes
        cmpct = HeaderAndShortIDs()
        cmpct.initialize_from_block(cmpct_block)
        via_cmpct = node.add_p2p_connection(P2PInterface())
        via_cmpct.send_and_ping(msg_cmpctblock(cmpct.to_p2p()))
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, above.hash)
        peers = {"first": first, "no_best": no_best, "other_best": other_best, "cmpctblock": via_cmpct}
        # the node asks with getheaders2 (compressed headers) or getheaders, whichever the peer takes
        asks = ("getheaders2", "getheaders")
        with p2p_lock:
            for p in peers.values():
                for ask in asks:
                    p.last_message.pop(ask, None)
        self.advance_clock(POS_TARGET_SPACING)  # the limit moves past the cut point
        def asked_from(p):
            with p2p_lock:
                return any(ask in p.last_message and p.last_message[ask].locator.vHave[0] == cut_point.sha256 for ask in asks)
        for name, p in peers.items():
            self.wait_until(lambda: asked_from(p), timeout=30)
            self.log.info("%s: asked again from the cut point at height %d", name, limit_now)
        # The compact block above the limit was this peer's only drop: its announcements on it do not count either.
        self.announce_on_dropped(node, via_cmpct, above, tag=0xcfff, what="a compact block above the limit")

        self.log.info("11. A rate, not a bound for the life of a connection: %d new branches, %d s on the same "
                      "connection, then as many again; one more within the window is not taken",
                      MAX_UNBACKED_FORKS_PER_PEER, UNBACKED_FORK_WINDOW)
        # Branches below the tip's work: the node fetches no block for them, so no download is in flight while the
        # clock moves a day (it would time out and drop the peer).
        rate_parent = node.getblockhash(tip["height"] - 2)
        rate_peer = node.add_p2p_connection(P2PInterface())
        before = self.entries(node)
        for i in range(MAX_UNBACKED_FORKS_PER_PEER):
            rate_peer.send_and_ping(msg_headers(self.branch(node, rate_parent, 1, tag=0xa000 + i)))
        assert_equal(self.entries(node) - before, MAX_UNBACKED_FORKS_PER_PEER)
        for _ in range(UNBACKED_FORK_WINDOW // CLOCK_HOP):
            self.advance_clock(CLOCK_HOP)  # in hops, a ping each, so that the connection stays active
            rate_peer.sync_with_ping()
        before = self.entries(node)
        for i in range(MAX_UNBACKED_FORKS_PER_PEER):
            rate_peer.send_and_ping(msg_headers(self.branch(node, rate_parent, 1, tag=0xb000 + i)))
        taken = self.entries(node) - before
        self.log.info("%d more new branches after the window: taken %d, connected=%s", MAX_UNBACKED_FORKS_PER_PEER, taken, rate_peer.is_connected)
        assert_equal(taken, MAX_UNBACKED_FORKS_PER_PEER)
        assert rate_peer.is_connected
        extra = self.branch(node, rate_parent, 1, tag=0xb000 + MAX_UNBACKED_FORKS_PER_PEER)[0]
        rate_peer.send_and_ping(msg_headers([extra]))
        self.log.info("one more within the window: taken %s, connected=%s",
                      extra.hash in [t["hash"] for t in node.getchaintips()], rate_peer.is_connected)
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, extra.hash)
        assert rate_peer.is_connected
        # That drop holds back the count for a day: announcements on it are not punished; a day later, headers from
        # the same peer that do not connect are punished as before.
        self.announce_on_dropped(node, rate_peer, extra, tag=0xb0ff, what="past the count after the window")
        for _ in range(UNBACKED_FORK_WINDOW // CLOCK_HOP + 1):
            self.advance_clock(CLOCK_HOP)
            rate_peer.sync_with_ping()
        unknown = self.branch(node, rate_parent, UNCONNECTING_ROUNDS + 1, tag=0xbf00)
        for header in unknown[1:]:
            rate_peer.send_message(msg_headers([header]))
        rate_peer.wait_for_disconnect()
        self.log.info("a day after the drop, %d headers that do not connect from the same peer: disconnected", UNCONNECTING_ROUNDS)

        self.log.info("12. Below the minimum chain work the height limit does not hold a node back: one %d blocks "
                      "behind syncs without the clock moving", MIN_WORK_BLOCKS)
        force_finish_mnsync(node)  # the clock jumped a day: the node restarted its sync
        assert_equal(node.setstaking(wallet_id), True)
        self.stake_to(node, node.getblockcount() + 1, POS_TARGET_SPACING)  # a fresh tip for both
        if not [p for p in fresh.getpeerinfo() if not p["inbound"]]:
            self.connect_nodes(2, 0)
        self.until_synced(fresh, node)
        self.disconnect_nodes(2, 0)
        self.stake_to(node, node.getblockcount() + MIN_WORK_BLOCKS, FAST_STEP)
        assert_equal(node.setstaking(wallet_id), False)
        time.sleep(3)
        target = node.getbestblockhash()
        limit = self.limit(fresh)
        self.log.info("the chain is at %d, the third node at %d; it would take headers without blocks up to %d",
                      node.getblockcount(), fresh.getblockcount(), limit)
        assert_greater_than(node.getblockcount(), limit)
        self.restart_node(2, extra_args=["-minimumchainwork=0x" + node.getblockchaininfo()["chainwork"]])
        fresh = self.nodes[2]
        fresh.setmocktime(self.mocktime)
        clock = self.mocktime
        self.connect_nodes(2, 0)
        started = time.time()
        self.wait_until(lambda: fresh.getbestblockhash() == target, timeout=120)
        self.log.info("synced to %d in %.0f s, the clock still at %d (unchanged: %s)",
                      fresh.getblockcount(), time.time() - started, self.mocktime, self.mocktime == clock)
        assert_equal(self.mocktime, clock)


if __name__ == '__main__':
    UnbackedPosHeadersTest().main()
