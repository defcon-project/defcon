#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A stake signed twice is noticed, counted once, and changes nothing else.

Two nodes. Node 0 stakes block 5001; node 1 follows. Copies of that block are
then offered to node 1 by hand:

  1  a copy signed again with the staker's key -- the same kernel, the same
     height, the same signer: a double signature. Counted, with one log line.
  2  the same copy again, and a third copy by the same signer: not counted again.
  3  two copies whose coinstake pays another key and is signed by it: anyone
     can write those about somebody else's kernel, as often as they like, and
     neither is signed by the kernel's owner, so they are not counted -- not even as a pair.

None of them replaces node 1's tip, node 0 counts nothing, and the chain goes on.
The staker's key is read with dumpprivkey, so this runs on a legacy wallet.
"""

import time

from test_framework.address import base58_to_byte, key_to_p2pkh
from test_framework.key import ECKey
from test_framework.messages import CBlock, from_hex, ser_uint256
from test_framework.script import CScript, OP_CHECKSIG
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, force_finish_mnsync, set_node_times

LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100
STAKE_CLOCK_STEP = 8
STAKE_CLOCK_SLEEP = 2
STAKE_TIMEOUT = 180


class PosDoubleSignTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            ["-staking=1", "-debug=pos"],
            ["-staking=0", "-debug=pos"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()
        if self.options.descriptors:
            raise SkipTest("reads the staker's key with dumpprivkey, which needs a legacy wallet")

    # ---- the clock and the staker -------------------------------------------

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def stake_one_block(self, node):
        """Stake exactly one block on node 0, then switch staking off and prove no second one came."""
        wallets = node.liststakingwallets()
        wallet_id = int(next(iter(wallets)))
        target = node.getblockcount() + 1
        assert_equal(node.setstaking(wallet_id), True)   # a toggle: True is "now on"
        deadline = time.time() + STAKE_TIMEOUT * self.options.timeout_factor
        while node.getblockcount() < target:
            assert time.time() < deadline, "no block staked; %s" % node.getstakinginfo()
            self.advance_clock(STAKE_CLOCK_STEP)
            poll_until = time.time() + STAKE_CLOCK_SLEEP
            while time.time() < poll_until and node.getblockcount() < target:
                time.sleep(0.2)
        assert_equal(node.setstaking(wallet_id), False)
        # the clock no longer moves, so no further search time opens
        time.sleep(4)
        assert_equal(node.getblockcount(), target)
        return node.getblockhash(target)

    # ---- copies of a staked block -------------------------------------------

    def staker_key(self, node, block):
        """The key CheckBlockSignature wants: the coinstake's second output pays it directly."""
        script = list(CScript(block.vtx[1].vout[1].scriptPubKey))
        assert_equal(len(script), 2)
        assert_equal(script[1], OP_CHECKSIG)
        pubkey = script[0]
        secret, _ = base58_to_byte(node.dumpprivkey(key_to_p2pkh(pubkey)))
        key = ECKey()
        key.set(secret[:32], True)
        assert_equal(key.get_pubkey().get_bytes(), pubkey)
        return key

    def copy(self, node, block_hash, tag, other_key=None):
        """The staked block with `tag` pushed onto its coinbase scriptSig (a proof-of-stake
        header keeps nNonce at 0), signed again by the staker -- or, with other_key, with the
        coinstake paying that key and signed by it."""
        block = from_hex(CBlock(), node.getblock(block_hash, 0))
        assert block.is_proof_of_stake()
        key = self.staker_key(node, block)
        coinbase = block.vtx[0]
        coinbase.vin[0].scriptSig = CScript(bytes(coinbase.vin[0].scriptSig) + bytes(CScript([tag])))
        coinbase.rehash()
        if other_key is not None:
            block.vtx[1].vout[1].scriptPubKey = CScript([other_key.get_pubkey().get_bytes(), OP_CHECKSIG])
            block.vtx[1].rehash()
            key = other_key
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.vchBlockSig = key.sign_ecdsa(ser_uint256(block.sha256))
        return block

    # ---- the test -----------------------------------------------------------

    def run_test(self):
        staker, observer = self.nodes
        if self.mocktime == 0:
            self.mocktime = staker.getblockheader(staker.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        mine_to = staker.getnewaddress()

        self.log.info("Proof of work up to %d", LAST_POW_BLOCK)
        while staker.getblockcount() < LAST_POW_BLOCK:
            todo = min(POW_CHUNK, LAST_POW_BLOCK - staker.getblockcount())
            self.advance_clock(POW_CHUNK_CLOCK_STEP)
            self.generatetoaddress(staker, todo, mine_to, sync_fun=self.sync_blocks)
        for node in self.nodes:
            force_finish_mnsync(node)
        tip_time = staker.getblockheader(staker.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)

        for node in self.nodes:
            assert_equal(node.getblockchaininfo()["stake_double_signs"], {"count": 0})

        self.log.info("Node 0 stakes block %d; node 1 follows", LAST_POW_BLOCK + 1)
        staked = self.stake_one_block(staker)
        self.sync_blocks()
        assert_equal(observer.getbestblockhash(), staked)
        coinstake = staker.getblock(staked, 2)["tx"][1]
        kernel = "%s:%d" % (coinstake["vin"][0]["txid"], coinstake["vin"][0]["vout"])
        assert_equal(observer.getblockchaininfo()["stake_double_signs"], {"count": 0})

        self.log.info("1: a second block by the same signer on the same kernel is counted, with one log line")
        second = self.copy(staker, staked, 1)
        with observer.assert_debug_log(["Double-signed stake: blocks %s and %s at height %d both spend kernel %s and are signed by its owner"
                                        % (staked, second.hash, LAST_POW_BLOCK + 1, kernel)]):
            assert_equal(observer.submitblock(second.serialize().hex()), "inconclusive")
        expected = {
            "count": 1,
            "last_height": LAST_POW_BLOCK + 1,
            "last_kernel": kernel,
            "last_blocks": [staked, second.hash],
        }
        assert_equal(observer.getblockchaininfo()["stake_double_signs"], expected)
        assert_equal(observer.getbestblockhash(), staked)

        self.log.info("2: the same block again, and a third one by the same signer, are not counted again")
        assert_equal(observer.submitblock(second.serialize().hex()), "duplicate")
        third = self.copy(staker, staked, 2)
        with observer.assert_debug_log(["one more block %s at height %d from the owner of kernel %s" % (third.hash, LAST_POW_BLOCK + 1, kernel)]):
            assert_equal(observer.submitblock(third.serialize().hex()), "inconclusive")
        assert_equal(observer.getblockchaininfo()["stake_double_signs"], expected)

        self.log.info("3: two blocks on the same kernel by one other key are not counted, not even as a pair")
        other = ECKey()
        other.generate()
        for tag in (3, 4):
            forged = self.copy(staker, staked, tag, other_key=other)
            with observer.assert_debug_log(["block %s at height %d names kernel %s under another key; not counted" % (forged.hash, LAST_POW_BLOCK + 1, kernel)]):
                assert_equal(observer.submitblock(forged.serialize().hex()), "inconclusive")
            assert_equal(observer.getblockchaininfo()["stake_double_signs"], expected)
        assert_equal(observer.getbestblockhash(), staked)

        self.log.info("Node 0 counted nothing, and the chain goes on from the block it staked")
        assert_equal(staker.getblockchaininfo()["stake_double_signs"], {"count": 0})
        following = self.stake_one_block(staker)
        self.sync_blocks()
        assert_equal(observer.getbestblockhash(), following)
        assert_equal(observer.getblockheader(following)["previousblockhash"], staked)
        assert_equal(observer.getblockchaininfo()["stake_double_signs"], expected)


if __name__ == "__main__":
    PosDoubleSignTest().main()
