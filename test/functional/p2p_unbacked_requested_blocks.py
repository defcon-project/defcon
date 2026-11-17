#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Staked blocks whose parent never comes: the node neither asks for them nor keeps them.

Above the last proof-of-work block a header carries no proof that stake is behind it, so the node takes a header
without its block only up to UNBACKED_HEADER_AHEAD heights above its tip, and a header that opens a new branch (on a
block that already has a child) counts against the peer (p2p_unbacked_pos_headers.py). The first child of the tip is
not a new branch, so any peer may put that many headers on the tip. Only ConnectBlock checks a block's stake, and a
block whose parent never comes is never connected; the block signature is the coinstake's own key's, so a peer can
sign a block on any kernel. Outside initial block download the node therefore asks a peer for such a block, and
processes one from a peer, only once the chain it builds on is complete here (BlockWaitsForParent and ProcessBlock
in net_processing.cpp): one block at a time on a chain whose blocks are still missing. Blocks that do not come from a
peer -- submitblock, -loadblock -- are taken as before, in any order, and so is a block asked for with
getblockfrompeer, from the moment it is asked for until the peer asked has answered or is gone.

Here a peer without stake builds a branch of full blocks (each just under the block size limit, signed with its own
key, the coinstake copied from the tip with the peer's key put in) on the tip, sends their headers and serves every
block the node asks for but the first. Each step logs its numbers before it checks them.

 1. On the tip, the first child (not counted as a new branch): the node asks for the first block only, keeps
    nothing, drops the peer once that block times out, and nothing marks the peer (no ban; it connects again). The
    same blocks sent without being asked for are not kept either.
 2. A new tip, a new first child: the same again. On mainnet a new tip comes every block interval.
 3. A second branch on the same tip, which opens a new branch and counts against the peer's network group: the same.
 4. Catching up: a second node misses CATCH_UP real blocks, more than a peer may have in flight, and syncs them
    one at a time from the first.
 5. A short reorganisation: the second node is on a block the first node then invalidates and replaces with a
    two-block branch; the second node moves to it.
 6. submitblock out of order: with the headers of two new blocks known, the second block submitted first is taken
    ("inconclusive"), and the first then connects both.
 7. getblockfrompeer out of order: with the headers of two more blocks known, the second is asked for from one peer.
    A copy with a damaged signature that another peer sends first is refused and does not use the request up: the
    block the asked peer then sends is kept, and the first then connects both. A block asked for from a peer that
    then leaves is, from another peer, a block nobody asked for again: not kept.
 8. -loadblock out of order: with the headers of two more blocks known, a bootstrap file holding the second before
    the first connects both.
 9. Initial block download: a third node syncs the whole chain from scratch, the staked blocks with the parallel
    download as before.
"""

import copy
import hashlib
import time

from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.messages import (MSG_BLOCK, MSG_CMPCT_BLOCK, CBlock, CBlockHeader, COutPoint, CTransaction, CTxIn,
                                     CTxOut, from_hex, msg_block, msg_headers, ser_uint256, uint256_from_compact)
from test_framework.p2p import MAGIC_BYTES, P2PInterface, p2p_lock
from test_framework.script import CScript, OP_CHECKSIG
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import FAST_STAKING_ARG, assert_equal, assert_greater_than, force_finish_mnsync, set_node_times

# chainparams.cpp, CRegTestParams (and the same on mainnet: posTargetSpacing, posTimestampMask)
LAST_POW_BLOCK = 5000
POS_TARGET_SPACING = 150
POS_TIMESTAMP_MASK = 5
POS_LIMIT = (1 << 224) - 1
STAKE_MIN_AGE = 10 * 60
# pow.cpp, Lwma3CalculateNextWorkRequired
LWMA_N = 36
# chain.h
MAX_FUTURE_BLOCK_TIME = 3 * 60
# net_processing.cpp
UNBACKED_HEADER_AHEAD = 24
UNBACKED_HEADER_AHEAD_PER_SPACING = 4
MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16
# net_processing.cpp: a block in flight longer than nPowTargetSpacing * BLOCK_DOWNLOAD_TIMEOUT_BASE (1) drops the peer
BLOCK_DOWNLOAD_TIMEOUT = POS_TARGET_SPACING
# consensus/consensus.h, MAX_DIP0001_BLOCK_SIZE; policy/policy.h, MAX_STANDARD_TX_SIZE (a block rule after DIP0001)
MAX_BLOCK_SIZE = 2000000
MAX_STANDARD_TX_SIZE = 100000
# Each block a little under the limit: room for the block signature and the transaction count's growth.
BLOCK_BYTES = MAX_BLOCK_SIZE - 2000
PAD_TX_BYTES = MAX_STANDARD_TX_SIZE - 1000

POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
# pos/minter.cpp under FAST_STAKING_ARG: a round every 0.1 s; a step of 8 s always opens a new search time
FAST_STEP = 8
STAKE_POLL = 6
STAKE_TIMEOUT = 300
STAKE_SETTLE = 3
# how long the node may go without asking for another block before a branch counts as settled
ASK_QUIET = 10
# step 4: more blocks than a peer may have in flight, and few enough that their headers all fit under the height limit
CATCH_UP = MAX_BLOCKS_IN_TRANSIT_PER_PEER + 4
SYNC_TIMEOUT = 120


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


def next_stake_time(previous):
    """The first time after `previous` a coinstake may carry (CheckCoinStakeTimestamp)."""
    t = previous + 1
    while t & POS_TIMESTAMP_MASK:
        t += 1
    return t


class BranchServer(P2PInterface):
    """Serves the blocks of its branch the node asks for, all but the withheld ones, and records what was asked."""

    def __init__(self, blocks, withheld):
        super().__init__()
        self.blocks = {b.sha256: b for b in blocks}
        self.withheld = set(withheld)
        self.asked = set()
        self.last_ask = time.time()

    def on_getdata(self, message):
        for inv in message.inv:
            if inv.type not in (MSG_BLOCK, MSG_CMPCT_BLOCK) or inv.hash not in self.blocks:
                continue
            if inv.hash not in self.asked:
                self.asked.add(inv.hash)
                self.last_ask = time.time()
            if inv.hash not in self.withheld:
                self.send_message(msg_block(self.blocks[inv.hash]))


class UnbackedRequestedBlocksTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        # The third node is in initial block download until its tip is within a minute of the clock: here the clock
        # stays within the default six hours of the genesis block (see the last step).
        self.extra_args = [["-staking=1", "-debug=net", "-debug=validation", FAST_STAKING_ARG], ["-debug=net"],
                           ["-debug=net", "-maxtipage=60"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(1, 0)  # the third node stays apart until the last step

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def wallet_id(self, node):
        return int(next(iter(node.liststakingwallets())))

    def set_staking(self, node, on):
        wallet_id = self.wallet_id(node)
        if node.getstakinginfo()[str(wallet_id)]["staking"] != on:
            assert_equal(node.setstaking(wallet_id), on)  # a toggle: the result is the new state
        assert_equal(node.getstakinginfo()[str(wallet_id)]["staking"], on)

    def stake_blocks(self, node, count):
        """At least `count` real blocks on the tip: staking on, the clock moved until they come, staking off again.
        At the fast pace a round already under way when staking goes off may still add one: the height is read once
        it has stood still for STAKE_SETTLE seconds."""
        target = node.getblockcount() + count
        self.set_staking(node, True)
        deadline = time.time() + count * STAKE_TIMEOUT * self.options.timeout_factor
        while node.getblockcount() < target:
            assert time.time() < deadline, "no block staked at height %d" % (node.getblockcount() + 1)
            before = node.getblockcount()
            self.advance_clock(FAST_STEP)
            poll_until = time.time() + STAKE_POLL
            while time.time() < poll_until and node.getblockcount() == before:
                time.sleep(0.2)
        self.set_staking(node, False)
        height, still_since = node.getblockcount(), time.time()
        while time.time() - still_since < STAKE_SETTLE:
            time.sleep(0.2)
            if node.getblockcount() != height:
                height, still_since = node.getblockcount(), time.time()
        assert height >= target
        return node.getbestblockhash()

    def tail(self, node, block_hash):
        entries = []
        for _ in range(LWMA_N + 1):
            header = node.getblockheader(block_hash)
            entries.append((header["time"], int(header["bits"], 16)))
            block_hash = header["previousblockhash"]
        return entries[::-1]

    def padding_tx(self, seed, size):
        """A transaction of about `size` bytes (OP_TRUE bytes, no signature operations) on an output that does not
        exist: CheckBlock does not look it up."""
        prevout = int.from_bytes(hashlib.sha256(seed).digest(), "little")
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(prevout, 0), b"", 0xffffffff)]
        tx.vout = [CTxOut(0, CScript(b"\x51" * (size - 64)))]
        tx.rehash()
        assert len(tx.serialize()) <= MAX_STANDARD_TX_SIZE
        return tx

    def full_branch(self, node, parent_hash, count, key, tag):
        """`count` full blocks in a row on `parent_hash`, without stake: each just under the block size limit, the
        tip's coinbase and coinstake copied with the coinstake paying `key`, and signed by it."""
        template = from_hex(CBlock(), node.getblock(node.getbestblockhash(), 0))
        assert template.is_proof_of_stake()
        parent = node.getblockheader(parent_hash)
        tail = self.tail(node, parent_hash)
        prev_hash, prev_time, height = int(parent_hash, 16), parent["time"], parent["height"]
        blocks = []
        for i in range(count):
            height += 1
            block = CBlock()
            block.nVersion = parent["version"]
            block.hashPrevBlock = prev_hash
            block.nTime = next_stake_time(prev_time)
            block.nBits = next_pos_bits(tail)
            block.nNonce = 0
            coinbase = copy.deepcopy(template.vtx[0])
            coinbase.vin[0].scriptSig = CScript([height, tag, i])
            coinbase.rehash()
            coinstake = copy.deepcopy(template.vtx[1])
            coinstake.vout[1].scriptPubKey = CScript([key.get_pubkey().get_bytes(), OP_CHECKSIG])
            coinstake.rehash()
            block.vtx = [coinbase, coinstake]
            room = BLOCK_BYTES - len(block.serialize())
            n = 0
            while room > 1000:
                tx = self.padding_tx(b"%d-%d-%d-%d" % (tag, height, i, n), min(PAD_TX_BYTES, room - 10))
                block.vtx.append(tx)
                room -= len(tx.serialize())
                n += 1
            block.hashMerkleRoot = block.calc_merkle_root()
            block.rehash()
            block.vchBlockSig = key.sign_ecdsa(ser_uint256(block.sha256))
            assert block.is_proof_of_stake()
            assert len(block.serialize()) <= MAX_BLOCK_SIZE
            blocks.append(block)
            tail = (tail + [(block.nTime, block.nBits)])[-(LWMA_N + 1):]
            prev_hash, prev_time = block.sha256, block.nTime
        assert blocks[-1].nTime <= self.mocktime + MAX_FUTURE_BLOCK_TIME
        return blocks

    def kept(self, node, block):
        """Whether the node stored this block's data (getblock refuses a block without it)."""
        try:
            node.getblock(block.hash, 1)
            return True
        except JSONRPCException:
            return False

    def blockfile_bytes(self, node):
        return sum(f.stat().st_size for f in (node.chain_path / "blocks").glob("blk*.dat"))

    def head_room(self, node):
        tip = node.getblockheader(node.getbestblockhash())
        behind = max(0, self.mocktime - tip["time"])
        return UNBACKED_HEADER_AHEAD + UNBACKED_HEADER_AHEAD_PER_SPACING * (behind // POS_TARGET_SPACING)

    def attack(self, node, parent_hash, key, tag, what):
        """A branch of full blocks on `parent_hash` from a fresh peer that withholds the first block; logs and checks
        what the node asked for and kept, and returns the peer and the blocks."""
        count = self.head_room(node) if parent_hash == node.getbestblockhash() else UNBACKED_HEADER_AHEAD
        blocks = self.full_branch(node, parent_hash, count, key, tag)
        files_before = self.blockfile_bytes(node)
        peer = node.add_p2p_connection(BranchServer(blocks, withheld=[blocks[0].sha256]))
        headers = []
        for b in blocks:
            header = CBlockHeader(b)
            header.rehash()
            headers.append(header)
        peer.send_and_ping(msg_headers(headers))
        # Until every block has been asked for, or the node has gone ASK_QUIET seconds without asking for another.
        wanted = {b.sha256 for b in blocks}
        while True:
            with p2p_lock:
                asked, quiet = len(peer.asked & wanted), time.time() - peer.last_ask
            if asked == len(wanted) or quiet > ASK_QUIET:
                break
            time.sleep(0.2)
        peer.sync_with_ping()
        best_header = node.getblockchaininfo()["headers"]
        kept = [b for b in blocks if self.kept(node, b)]
        self.log.info("%s: %d headers taken of %d sent (best header %d, tip %d); the node asked for %d blocks, the "
                      "first withheld; it kept %d of the %d served; the block files grew %d bytes", what,
                      best_header - node.getblockcount(), len(blocks), best_header, node.getblockcount(), asked,
                      len(kept), len(blocks) - 1, self.blockfile_bytes(node) - files_before)
        assert_equal(best_header - node.getblockcount(), len(blocks))
        with p2p_lock:
            assert_equal(peer.asked & wanted, {blocks[0].sha256})
        assert_equal(kept, [])
        return peer, blocks

    def two_new_blocks(self, node, receiver):
        """Two real blocks staked by `node` while `receiver` is apart; `receiver` gets their headers (submitheader)
        and not their data. A block staked past the count before (stake_blocks) is handed to `receiver` first."""
        for height in range(receiver.getblockcount() + 1, node.getblockcount() + 1):
            assert_equal(receiver.submitblock(node.getblock(node.getblockhash(height), 0)), None)
        assert_equal(receiver.getbestblockhash(), node.getbestblockhash())
        tip = node.getblockcount()
        self.stake_blocks(node, 2)
        blocks = []
        for height in (tip + 1, tip + 2):
            block = from_hex(CBlock(), node.getblock(node.getblockhash(height), 0))
            block.rehash()
            blocks.append(block)
        for b in blocks:
            receiver.submitheader(CBlockHeader(b).serialize().hex())
            assert not self.kept(receiver, b)
        return blocks

    def only_peer_id(self, node):
        peers = node.getpeerinfo()
        assert_equal(len(peers), 1)
        return peers[0]["id"]

    def until_synced(self, a, b):
        self.wait_until(lambda: a.getbestblockhash() == b.getbestblockhash(), timeout=SYNC_TIMEOUT)

    def run_test(self):
        node, other, fresh = self.nodes
        self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
        set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining to the proof-of-stake boundary (height %d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            # the second node holds every block before the clock moves on
            self.generatetoaddress(node, min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount()), address,
                                   sync_fun=lambda: self.sync_blocks([node, other]))
        force_finish_mnsync(node)
        self.set_staking(node, False)
        self.advance_clock(STAKE_MIN_AGE)
        tip_hash = self.stake_blocks(node, 1)
        self.sync_blocks([node, other])
        self.disconnect_nodes(1, 0)
        self.log.info("A real staked tip at %d; initial block download: %s", node.getblockcount(),
                      node.getblockchaininfo()["initialblockdownload"])
        assert not node.getblockchaininfo()["initialblockdownload"]

        key = ECKey()
        key.generate()

        self.log.info("1. A branch of full blocks on the tip, the first block withheld")
        peer, blocks = self.attack(node, tip_hash, key, 0x51, "on the tip")
        self.advance_clock(BLOCK_DOWNLOAD_TIMEOUT + 1)
        peer.wait_for_disconnect(timeout=30 * self.options.timeout_factor)
        banned = node.listbanned()
        unasked = node.add_p2p_connection(P2PInterface())
        self.log.info("after %d s with the first block in flight the node dropped the peer; banned: %s; a new "
                      "connection from the same address is taken: %s", BLOCK_DOWNLOAD_TIMEOUT + 1, banned,
                      unasked.is_connected)
        assert_equal(banned, [])
        assert unasked.is_connected
        for b in blocks[1:]:
            unasked.send_message(msg_block(b))
        unasked.sync_with_ping()
        kept = [b for b in blocks if self.kept(node, b)]
        self.log.info("the same blocks but the first sent without being asked for: %d kept", len(kept))
        assert_equal(kept, [])
        node.disconnect_p2ps()

        self.log.info("2. A new real tip, and a branch on it the same way")
        tip_hash = self.stake_blocks(node, 1)
        self.attack(node, tip_hash, key, 0x52, "on the next tip")
        node.disconnect_p2ps()

        self.log.info("3. A second branch on the same tip (a new branch, counted against the group)")
        self.attack(node, tip_hash, key, 0x53, "a second branch on the tip")
        node.disconnect_p2ps()

        self.log.info("4. Catching up: the second node misses %d real blocks", CATCH_UP)
        self.stake_blocks(node, CATCH_UP)
        assert not other.getblockchaininfo()["initialblockdownload"]
        start = time.time()
        self.connect_nodes(1, 0)
        self.until_synced(other, node)
        self.log.info("the second node caught up to %d in %.1f s", other.getblockcount(), time.time() - start)

        self.log.info("5. A short reorganisation onto a two-block branch")
        replaced = self.stake_blocks(node, 1)
        self.sync_blocks([node, other])
        self.disconnect_nodes(1, 0)
        node.invalidateblock(replaced)
        branch_tip = self.stake_blocks(node, 2)
        assert_equal(other.getbestblockhash(), replaced)
        self.connect_nodes(1, 0)
        self.until_synced(other, node)
        self.log.info("the second node moved from %s to the branch's tip %s at %d", replaced[:16], branch_tip[:16],
                      other.getblockcount())
        assert_equal(other.getbestblockhash(), branch_tip)

        self.log.info("6. submitblock out of order, the headers known")
        self.disconnect_nodes(1, 0)
        first, second = self.two_new_blocks(node, other)
        result = other.submitblock(second.serialize().hex())
        stored = self.kept(other, second)
        self.log.info("the second block submitted first: %s, stored: %s", result, stored)
        assert_equal(result, "inconclusive")
        assert stored
        assert_equal(other.submitblock(first.serialize().hex()), None)
        self.log.info("the first block then: the second node's tip is the second block: %s",
                      other.getbestblockhash() == second.hash)
        assert_equal(other.getbestblockhash(), second.hash)

        self.log.info("7. getblockfrompeer out of order, the headers known")
        assert not other.getblockchaininfo()["initialblockdownload"]
        first, second = self.two_new_blocks(node, other)
        tip_before = other.getbestblockhash()
        asked = other.add_p2p_connection(P2PInterface())
        assert_equal(other.getblockfrompeer(second.hash, self.only_peer_id(other)), {})
        asked.wait_for_getdata([second.sha256])
        damaged = copy.deepcopy(second)
        signature = bytearray(damaged.vchBlockSig)
        signature[len(signature) // 2] ^= 0x01
        damaged.vchBlockSig = bytes(signature)
        damaged.rehash()
        assert_equal(damaged.hash, second.hash)
        with other.assert_debug_log(["bad-blk-sign"], timeout=30):
            other.add_p2p_connection(P2PInterface()).send_message(msg_block(damaged))
        kept_damaged = self.kept(other, second)
        asked.send_and_ping(msg_block(second))
        stored = self.kept(other, second)
        self.log.info("the second block asked for from one peer; a copy with a damaged signature from another peer "
                      "first, kept: %s; then the block from the peer asked, kept: %s", kept_damaged, stored)
        assert not kept_damaged
        assert stored
        assert_equal(other.getbestblockhash(), tip_before)
        assert_equal(other.submitblock(first.serialize().hex()), None)
        self.log.info("the first block then: the second node's tip is the second block: %s",
                      other.getbestblockhash() == second.hash)
        assert_equal(other.getbestblockhash(), second.hash)
        other.disconnect_p2ps()
        # The request ends with the peer it was made to: the same block from another peer is one nobody asked for.
        first, second = self.two_new_blocks(node, other)
        asked = other.add_p2p_connection(P2PInterface())
        asked_id = self.only_peer_id(other)
        assert_equal(other.getblockfrompeer(second.hash, asked_id), {})
        asked.wait_for_getdata([second.sha256])
        with other.assert_debug_log(["Cleared nodestate for peer=%d\n" % asked_id], timeout=30):
            asked.peer_disconnect()
        other.add_p2p_connection(P2PInterface()).send_and_ping(msg_block(second))
        kept_unasked = self.kept(other, second)
        self.log.info("a block asked for from a peer that then left, sent by another peer: kept: %s", kept_unasked)
        assert not kept_unasked
        other.disconnect_p2ps()
        for b in (first, second):
            assert_equal(other.submitblock(b.serialize().hex()), None)
        assert_equal(other.getbestblockhash(), second.hash)

        self.log.info("8. -loadblock out of order, the headers known")
        first, second = self.two_new_blocks(node, other)
        bootstrap = self.options.tmpdir + "/out_of_order.dat"
        with open(bootstrap, "wb") as f:
            for b in (second, first):
                raw = b.serialize()
                f.write(MAGIC_BYTES[self.chain] + len(raw).to_bytes(4, "little") + raw)
        self.stop_node(1)
        self.start_node(1, extra_args=self.extra_args[1] + ["-loadblock=" + bootstrap])
        set_node_times(self.nodes, self.mocktime)
        self.wait_until(lambda: other.getbestblockhash() == second.hash, timeout=SYNC_TIMEOUT)
        self.log.info("a bootstrap file with the second block before the first: the second node's tip is the second "
                      "block at %d", other.getblockcount())

        self.log.info("9. Initial block download: a third node syncs from scratch")
        # It left initial block download as it started, with the clock at the genesis block (the state latches): a
        # restart now, with the clock far past it, puts it back in.
        self.restart_node(2)
        set_node_times(self.nodes, self.mocktime)
        assert fresh.getblockchaininfo()["initialblockdownload"]
        start = time.time()
        self.connect_nodes(2, 0)
        self.until_synced(fresh, node)
        self.log.info("the third node synced %d blocks in %.1f s", fresh.getblockcount(), time.time() - start)
        assert_greater_than(fresh.getblockcount(), LAST_POW_BLOCK)


if __name__ == '__main__':
    UnbackedRequestedBlocksTest().main()
