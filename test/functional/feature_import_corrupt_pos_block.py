#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A damaged proof-of-stake block on import must not mark its header invalid.

-reindex and -loadblock hand each block to AcceptBlock, which marks the header
invalid for good on any CheckBlock failure that is not BLOCK_MUTATED. A
proof-of-stake block's signature is committed to neither by the header hash nor
by the merkle root, and whether the proof of work is checked is decided by the
body (a coinstake in vtx[1]) while the hash only knows nNonce. So a damaged copy
of a good block, with the very same hash, used to mark that hash invalid: the
node then refused the good copy from its peers and stalled at the block before.

Three damaged copies of a staked block, each with the good block's hash:

  a. one byte of the signature flipped (-loadblock);
  b. the coinstake made a plain transaction without touching the merkle root,
     so the body reads as proof of work and, against the block's stake target,
     fails the proof of work (-loadblock);
  c. a valid signature lengthened past the block size limit -- the lax DER
     parser still accepts it -- reached through -reindex: the copy sits before
     its parent in the node's own block file under an 80-byte record, and is
     read back whole once the parent is in.

For each the node must stay at the parent with the block's header not marked
invalid, also after a restart, and then take the good block from its peer.
--case runs one of them alone (the negative control runs each on the base).
"""

import os
import struct
import time
from io import BytesIO

from test_framework.messages import (
    CBlock,
    CTxOut,
    MAGIC_BYTES,
)
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    force_finish_mnsync,
    set_node_times,
)

# chainparams.cpp, CRegTestParams; consensus/consensus.h
LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
MAX_BLOCK_SIZE = 2000000

# mock-clock discipline, as in feature_pos_staking.py and feature_pos_verifychain.py
POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
STAKE_CLOCK_STEP = 64
STAKE_TIMEOUT = 300

CASES = ("a", "b", "c")


def target_from_bits(bits):
    return (bits & 0x7fffff) << (8 * ((bits >> 24) - 3))


class ImportCorruptPosBlockTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--case", dest="case", default="all", choices=CASES + ("all",),
                            help="run one damaged copy only (default: all three)")

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-staking=1", "-debug=pos"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # node1 only meets node0 when the test says so
        self.setup_nodes()

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def staking_info(self):
        return self.nodes[0].getstakinginfo()[str(self.wallet_id)]

    def stake_one_more(self):
        """Stake on node0 until its tip is past the current one, then stop staking; returns the first new block's hash."""
        node = self.nodes[0]
        start = node.getblockcount()
        assert_equal(node.setstaking(self.wallet_id), True)
        self.wait_until(lambda: self.staking_info()["minter_running"], timeout=30)
        deadline = time.time() + STAKE_TIMEOUT * self.options.timeout_factor
        while node.getblockcount() <= start:
            if time.time() > deadline:
                raise AssertionError("no block staked within %ds; getstakinginfo: %s" % (STAKE_TIMEOUT, self.staking_info()))
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        assert_equal(node.setstaking(self.wallet_id), False)
        # let a block that was being staked when staking stopped land before going on
        time.sleep(2)
        blockhash = node.getblockhash(start + 1)
        assert_equal(node.getblock(blockhash)["flags"], "proof-of-stake")
        return blockhash

    def sync_node1_and_part(self):
        """node1 takes node0's chain, then the two part again."""
        self.connect_nodes(1, 0)
        self.sync_blocks(timeout=600)
        self.disconnect_nodes(1, 0)

    def block_of(self, blockhash):
        block = CBlock()
        block.deserialize(BytesIO(bytes.fromhex(self.nodes[0].getblock(blockhash, 0))))
        block.rehash()
        assert_equal(block.hash, blockhash)
        return block

    def wait_for_log(self, node, text, since, timeout=120):
        def found():
            with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
                f.seek(since)
                return text in f.read()
        self.wait_until(found, timeout=timeout)

    def record(self, payload, length=None):
        return MAGIC_BYTES[self.chain] + struct.pack("<I", len(payload) if length is None else length) + payload

    def assert_not_marked(self, blockhash):
        for tip in self.nodes[1].getchaintips():
            if tip["hash"] == blockhash:
                assert tip["status"] != "invalid", "the damaged copy marked the header invalid: %s" % tip
        assert_equal(self.nodes[1].getbestblockhash(), self.nodes[0].getblockheader(blockhash)["previousblockhash"])

    def takes_the_good_block(self, blockhash):
        self.connect_nodes(1, 0)
        self.wait_until(lambda: self.nodes[1].getbestblockhash() == self.nodes[0].getbestblockhash(), timeout=120)
        assert_equal(self.nodes[1].getblockheader(blockhash)["confirmations"] > 0, True)
        self.disconnect_nodes(1, 0)

    def run_test(self):
        node = self.nodes[0]
        self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
        set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()

        self.log.info("Mining proof-of-work blocks up to lastPowBlock (%d)", LAST_POW_BLOCK)
        while node.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - node.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(node, todo, address, sync_fun=self.no_op)
        force_finish_mnsync(node)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        self.wallet_id = int(next(iter(wallets)))

        self.log.info("node1 takes the chain, then node0 stakes a first proof-of-stake block")
        self.stake_one_more()
        self.sync_node1_and_part()

        cases = CASES if self.options.case == "all" else (self.options.case,)
        for case in cases:
            getattr(self, "case_" + case)()

    def import_and_check(self, label, blockhash, damaged, reason):
        node1 = self.nodes[1]
        path = os.path.join(self.options.tmpdir, "damaged_%s.dat" % label)
        with open(path, "wb") as f:
            f.write(self.record(damaged.serialize()))
        since = os.path.getsize(node1.debug_log_path)
        with node1.assert_debug_log([reason]):
            self.restart_node(1, extra_args=["-loadblock=%s" % path])
            self.wait_for_log(node1, "blocks from external file", since)
        self.assert_not_marked(blockhash)
        self.log.info("  after a restart the header is still not marked invalid")
        self.restart_node(1)
        self.assert_not_marked(blockhash)
        self.log.info("  node1 takes the good block from node0")
        self.takes_the_good_block(blockhash)

    def case_a(self):
        self.log.info("a. One byte of the signature flipped")
        blockhash = self.stake_one_more()
        damaged = self.block_of(blockhash)
        sig = bytearray(damaged.vchBlockSig)
        sig[len(sig) // 2] ^= 0x01
        damaged.vchBlockSig = bytes(sig)
        damaged.rehash()
        assert_equal(damaged.hash, blockhash)
        self.import_and_check("a", blockhash, damaged, "bad-blk-sign")

    def case_b(self):
        self.log.info("b. The coinstake made a plain transaction, the merkle root left as it was")
        blockhash = self.stake_one_more()
        damaged = self.block_of(blockhash)
        # the precondition: against the block's own (stake) target the hash misses, so a
        # body that reads as proof of work fails the proof of work on the base
        assert_greater_than(int(blockhash, 16), target_from_bits(damaged.nBits))
        damaged.vtx[1].vout[0] = CTxOut(1, CScript([OP_TRUE]))
        damaged.vtx[1].rehash()
        assert not damaged.is_proof_of_stake()
        damaged.rehash()
        assert_equal(damaged.hash, blockhash)
        self.import_and_check("b", blockhash, damaged, "bad-txnmrklroot")

    def case_c(self):
        self.log.info("c. A valid signature lengthened past the block size limit, on -reindex")
        node1 = self.nodes[1]
        blockhash = self.stake_one_more()
        parent = self.nodes[0].getblockheader(blockhash)["previousblockhash"]
        assert_equal(node1.getbestblockhash(), parent)
        damaged = self.block_of(blockhash)
        damaged.vchBlockSig += b"\x00" * MAX_BLOCK_SIZE
        damaged.rehash()
        assert_equal(damaged.hash, blockhash)
        payload = damaged.serialize()
        assert_greater_than(len(payload), MAX_BLOCK_SIZE)

        self.log.info("  the copy goes before its parent in node1's own block file, under an 80-byte record")
        parent_bytes = bytes.fromhex(node1.getblock(parent, 0))
        self.stop_node(1)
        blk = os.path.join(node1.datadir, self.chain, "blocks", "blk00000.dat")
        with open(blk, "rb") as f:
            data = f.read()
        parent_record = self.record(parent_bytes)
        at = data.find(parent_record)
        assert at > 0 and data.find(parent_record, at + 1) == -1, "the parent's record is not in blk00000.dat exactly once"
        with open(blk, "wb") as f:
            f.write(data[:at] + self.record(payload, length=80) + data[at:])

        since = os.path.getsize(node1.debug_log_path)
        with node1.assert_debug_log(["bad-blk-length"], timeout=120):
            self.start_node(1, extra_args=["-reindex"])
            self.wait_for_log(node1, "Reindexing finished", since, timeout=300)
            self.wait_until(lambda: node1.getbestblockhash() == parent, timeout=120)
        self.assert_not_marked(blockhash)
        self.log.info("  after a restart the header is still not marked invalid")
        self.restart_node(1)
        self.assert_not_marked(blockhash)
        self.log.info("  node1 takes the good block from node0")
        self.takes_the_good_block(blockhash)


if __name__ == "__main__":
    ImportCorruptPosBlockTest().main()
