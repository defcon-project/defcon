#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regress the masternode-sync guard against headers without backing blocks.

The defensive case is documented in
reports/staking/delegated-staking-centralization.md, section 10.2.

ContextualCheckBlockHeader() does not run CheckProofOfWork() once the previous
height is past lastPowBlock (validation.cpp, the require_pow flag before it is
called). Above that height a header only has to carry a zero nonce, a time
strictly after the previous block, the nBits value GetNextWorkRequired()
computes, and a current version (ContextualCheckBlockHeader, bad-pos-nonce /
bad-pos-time / bad-diffbits / bad-version). None of this names a transaction,
so the header can be built without a block behind it.

This defensive regression test adds a few such headers on top of a staking
node's tip, without ever sending the matching block. It checks whether they
remain the best header after restart, hold masternode sync in its blockchain
stage, and prevent the minter from producing blocks. A clean restart before
the headers is the control: with a peer present, sync passes the blockchain
stage and the same wallet stakes again.
"""

import time

from test_framework.messages import CBlockHeader, msg_headers, uint256_from_compact
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync, set_node_times

# chainparams.cpp, CRegTestParams
LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
STAKE_CLOCK_STEP = 64
STAKE_TIMEOUT = 300
SYNC_CONTROL_TIMEOUT = 60
SYNC_STALL_SECONDS = 20
STAKE_STALL_SECONDS = 20
HEIGHT_QUIET_SECONDS = 4
HEIGHT_SETTLE_TIMEOUT = 60
POS_TARGET_SPACING = 150
POS_LIMIT = (1 << 224) - 1  # posLimit, regtest

POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100

# pow.cpp, Lwma3CalculateNextWorkRequired
LWMA_N = 36

# Kept small on purpose: enough to show the header is accepted and kept, not a
# measurement of how many a peer could send.
HEADER_COUNT = 3


def compact_from_uint256(value):
    """Inverse of test_framework.messages.uint256_from_compact: the same 3-byte
    plus exponent encoding arith_uint256::GetCompact() produces."""
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
    """Port of Lwma3CalculateNextWorkRequired (pow.cpp): the nBits a block
    right after chain[-1] must carry. `chain` holds (time, nBits) for the
    previous LWMA_N + 1 blocks, oldest first -- the same window the real
    function reads from the block index."""
    k = LWMA_N * (LWMA_N + 1) * POS_TARGET_SPACING // 2
    previous_time = chain[0][0]
    weighted, sum_target = 0, 0
    for j, (block_time, bits) in enumerate(chain[1:], start=1):
        this_time = block_time if block_time > previous_time else previous_time + 1
        weighted += min(6 * POS_TARGET_SPACING, this_time - previous_time) * j
        previous_time = this_time
        sum_target += uint256_from_compact(bits) // (k * LWMA_N)
    return compact_from_uint256(min(weighted * sum_target, POS_LIMIT))


class PosHeaderOnlyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=1", "-debug=pos", "-debug=mnsync"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.descriptors:
            self.skip_if_no_sqlite()
        else:
            self.skip_if_no_bdb()

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def mine_to_boundary(self, node, address):
        while node.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)

    def stake_one_block(self, node, timeout=STAKE_TIMEOUT):
        target_height = node.getblockcount() + 1
        deadline = time.time() + timeout * self.options.timeout_factor
        while time.time() < deadline:
            if node.getblockcount() >= target_height:
                return target_height
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        raise AssertionError("no block staked at height %d within %ds" % (target_height, timeout))

    def staking_info(self, node, wallet_id):
        return node.getstakinginfo()[str(wallet_id)]

    def enable_staking(self, node):
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        wallet_id = int(next(iter(wallets)))
        if not self.staking_info(node, wallet_id)["staking"]:
            assert_equal(node.setstaking(wallet_id), True)
        self.wait_until(lambda: self.staking_info(node, wallet_id)["minter_running"], timeout=30)
        return wallet_id

    def disable_staking(self, node, wallet_id):
        if self.staking_info(node, wallet_id)["staking"]:
            assert_equal(node.setstaking(wallet_id), False)
        assert_equal(self.staking_info(node, wallet_id)["staking"], False)

    def settled_height(self, node):
        """Read the active height only after staking has been switched off."""
        height = node.getblockcount()
        quiet_since = time.time()
        deadline = time.time() + HEIGHT_SETTLE_TIMEOUT * self.options.timeout_factor
        while time.time() < deadline:
            time.sleep(0.5)
            current = node.getblockcount()
            if current != height:
                height, quiet_since = current, time.time()
            elif time.time() - quiet_since >= HEIGHT_QUIET_SECONDS:
                return height
        raise AssertionError("block height did not settle after staking was switched off")

    def wait_for_blockchain_sync(self, node):
        """Wait for the real header-tip guard to pass its blockchain stage."""
        deadline = time.time() + SYNC_CONTROL_TIMEOUT * self.options.timeout_factor
        while time.time() < deadline:
            status = node.mnsync("status")
            if status["IsBlockchainSynced"]:
                return status
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        raise AssertionError("clean control restart did not pass blockchain sync: %s"
                             % node.mnsync("status"))

    def assert_sync_stays_pending(self, node):
        """Give the sync ticker several chances to pass; a best-header gap must hold it."""
        for _ in range(SYNC_STALL_SECONDS):
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
            status = node.mnsync("status")
            assert_equal(status["IsBlockchainSynced"], False)
            assert_equal(status["IsSynced"], False)

    def recent_chain(self, node, block_hash):
        """(time, nBits) of block_hash and the LWMA_N blocks before it, oldest
        first -- the window next_pos_bits() needs, read once from real blocks."""
        entries = []
        for _ in range(LWMA_N + 1):
            header = node.getblockheader(block_hash)
            entries.append((header["time"], int(header["bits"], 16)))
            block_hash = header["previousblockhash"]
        return entries[::-1]

    def run_test(self):
        node = self.nodes[0]
        if self.mocktime == 0:
            self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining to the proof-of-stake boundary (height %d)", LAST_POW_BLOCK)
        self.mine_to_boundary(node, address)

        force_finish_mnsync(node)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)

        wallet_id = self.enable_staking(node)
        first_stake_height = node.getblockcount() + 1
        self.log.info("Staking one real block at height %d before the header-only part", first_stake_height)
        assert_equal(self.stake_one_block(node), first_stake_height)
        self.disable_staking(node, wallet_id)
        self.settled_height(node)

        self.log.info("Control: restart before any unbacked headers are sent")
        self.restart_node(0)
        set_node_times(self.nodes, self.mocktime)
        control_chain = node.getblockchaininfo()
        assert_equal(control_chain["headers"], control_chain["blocks"])
        self.log.info("Control chain has no header-only gap: blocks=%d headers=%d",
                      control_chain["blocks"], control_chain["headers"])
        control_peer = node.add_p2p_connection(P2PInterface())
        control_sync = self.wait_for_blockchain_sync(node)
        self.log.info("Control sync passed the blockchain stage: %s", control_sync["AssetName"])
        force_finish_mnsync(node)
        control_sync = node.mnsync("status")
        assert_equal(control_sync["IsBlockchainSynced"], True)
        assert_equal(control_sync["IsSynced"], True)

        wallet_id = self.enable_staking(node)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        self.mocktime = max(self.mocktime, tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP)
        set_node_times(self.nodes, self.mocktime)
        control_stake_height = node.getblockcount() + 1
        self.log.info("Control staking resumes at height %d", control_stake_height)
        assert_equal(self.stake_one_block(node), control_stake_height)
        self.disable_staking(node, wallet_id)
        blocks_before = self.settled_height(node)

        tip = node.getbestblockhash()
        headers_before = node.getblockchaininfo()["headers"]
        assert_equal(blocks_before, headers_before)

        self.log.info("Sending %d headers above the tip, with no block behind any of them", HEADER_COUNT)
        chain_tail = self.recent_chain(node, tip)
        version = node.getblockheader(tip)["version"]
        prev_hash, prev_time = int(tip, 16), chain_tail[-1][0]
        headers = []
        for _ in range(HEADER_COUNT):
            header = CBlockHeader()
            header.nVersion = version
            header.hashPrevBlock = prev_hash
            header.hashMerkleRoot = 0
            header.nTime = prev_time + 1
            header.nBits = next_pos_bits(chain_tail)
            header.nNonce = 0
            header.rehash()
            headers.append(header)
            chain_tail = (chain_tail + [(header.nTime, header.nBits)])[-(LWMA_N + 1):]
            prev_hash, prev_time = header.sha256, header.nTime

        control_peer.send_and_ping(msg_headers(headers))

        info = node.getblockchaininfo()
        self.log.info("blocks=%d headers=%d (before: blocks=%d headers=%d)",
                      info["blocks"], info["headers"], blocks_before, headers_before)
        assert_equal(info["blocks"], blocks_before)
        assert_equal(info["headers"], headers_before + HEADER_COUNT)

        tips = node.getchaintips()
        header_only_tips = [t for t in tips if t["status"] == "headers-only"]
        self.log.info("chain tips: %s", [(t["height"], t["status"]) for t in tips])
        assert_equal(len(header_only_tips), 1)
        assert_equal(header_only_tips[0]["height"], blocks_before + HEADER_COUNT)
        assert_equal(header_only_tips[0]["branchlen"], HEADER_COUNT)

        node.disconnect_p2ps()
        self.restart_node(0)
        set_node_times(self.nodes, self.mocktime)
        info_after_restart = node.getblockchaininfo()
        self.log.info("after restart: blocks=%d headers=%d", info_after_restart["blocks"], info_after_restart["headers"])
        assert_equal(info_after_restart["blocks"], blocks_before)
        assert_equal(info_after_restart["headers"], headers_before + HEADER_COUNT)

        header_sync_peer = node.add_p2p_connection(P2PInterface())
        sync_status = node.mnsync("status")
        assert_equal(sync_status["AssetName"], "MASTERNODE_SYNC_BLOCKCHAIN")
        self.assert_sync_stays_pending(node)
        sync_status = node.mnsync("status")
        self.log.info("Header observation: active height=%d, best header=%d; sync remains %s, IsSynced=%s",
                      info_after_restart["blocks"], info_after_restart["headers"],
                      sync_status["AssetName"], sync_status["IsSynced"])
        assert_equal(sync_status["IsBlockchainSynced"], False)
        assert_equal(sync_status["IsSynced"], False)

        wallet_id = self.enable_staking(node)
        assert_equal(self.staking_info(node, wallet_id)["staking"], True)
        assert_equal(self.staking_info(node, wallet_id)["minter_running"], True)
        blocked_height = node.getblockcount()
        self.log.info("Staking observation: wallet enabled; checking that no block is produced")
        for _ in range(STAKE_STALL_SECONDS):
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
            assert_equal(node.getblockcount(), blocked_height)
            assert_equal(node.mnsync("status")["IsSynced"], False)
        self.log.info("Staking stayed blocked at height %d while masternode sync was unfinished", blocked_height)
        self.disable_staking(node, wallet_id)


if __name__ == '__main__':
    PosHeaderOnlyTest().main()
