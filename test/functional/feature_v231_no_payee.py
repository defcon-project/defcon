#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""From the v23.1 height a staked block with no payable masternode carries no payout at all.

The network has one masternode, and it is revoked before the height, so every
block from then on has masternodes registered and none payable. The honest staker
then puts nothing in the coinbase. Three nodes:

  node 0  stakes, and is cut off from the others while it stakes H - 1 and H, so
          each of its blocks can be offered to them by hand, a forged copy first
  node 1  runs without the height: a binary from before the release
  node 2  the masternode's node, running with the height

What it shows:

  1  Below H every honest block has an empty coinbase, and a copy of H - 1 that
     pays 5000 to the staker is taken by the node with the height, and so is a
     copy that adds only a data output of no value: that is the gap, and this
     binary has it until the height.
  2  At H both copies are refused by the node with the height, while the node
     without it takes the paying copy and keeps it as its tip: the extra payout
     was the only thing wrong with it.
  3  The honest block H has an empty coinbase and is taken by the node with the
     height, and H + 1 follows. The honest block breaks no rule of the node without
     the height, so that node goes back to the honest chain once it is longer: one
     forged block does not split an old binary off for good.
  4  -reindex revalidates the chain across H.

What it does not show: a real network, more than one staker, the platform share
(mn_rr is not scheduled on any release network and is pushed out of reach here; the
unit cases force it).
"""

import time
from decimal import Decimal

from test_framework.address import base58_to_byte, key_to_p2pkh
from test_framework.key import ECKey
from test_framework.messages import COIN, CBlock, CTxOut, from_hex, ser_uint256
from test_framework.script import CScript, OP_CHECKSIG, OP_RETURN
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync, set_node_times

LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
MASTERNODE_COLLATERAL = 1000
EXTRA = Decimal("5000")
H = 5016
NOT_IN_THIS_RUN = 6000000
NEVER = 2**31 - 1              # the largest height stands for never, as an unset gate holds it

MINE_CHUNK = 200
SYNC_TIMEOUT = 300
REINDEX_TIMEOUT = 900
STAKE_CLOCK_STEP = 8
LAST_BLOCK_CLOCK_STEP = 2
STAKE_CLOCK_SLEEP = 2
STAKE_TIMEOUT = 180
TX_AGE = 130                   # WAIT_FOR_ISLOCK_TIMEOUT (120 s) plus a margin: no quorum exists to lock a transaction


class V231NoPayeeTest(DashTestFramework):
    def set_test_params(self):
        rules = ["-testactivationheight=v20@%d" % NOT_IN_THIS_RUN,
                 "-testactivationheight=mn_rr@%d" % NEVER,
                 "-peertimeout=999999999",
                 "-staking=0"]
        height = ["-testactivationheight=v231@%d" % H,
                  "-testactivationheight=superblocksretired@%d" % H]
        self.set_dash_test_params(3, 1, extra_args=[
            rules + height + ["-staking=1", "-txindex=1", "-debug=pos"],
            rules,
            rules + height,
        ])

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ---- the clock and the staker -------------------------------------------

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def staking_info(self, node):
        return node.getstakinginfo()[str(self.wallet_id)]

    def enable_staking(self, node):
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        self.wallet_id = int(next(iter(wallets)))
        assert_equal(node.setstaking(self.wallet_id), True)
        self.wait_until(lambda: self.staking_info(node)["minter_running"], timeout=30)

    def settled_height(self, node, quiet=4, timeout=90):
        height = node.getblockcount()
        quiet_since = time.time()
        deadline = time.time() + timeout * self.options.timeout_factor
        while time.time() < deadline:
            time.sleep(0.5)
            current = node.getblockcount()
            if current != height:
                height, quiet_since = current, time.time()
            elif time.time() - quiet_since >= quiet:
                return height
        raise AssertionError("the block count kept moving after staking was switched off")

    def stake_exactly_to(self, node, height):
        """Stake until the chain holds `height`, then switch staking off and prove not one more arrived."""
        self.enable_staking(node)
        last_progress = deadline = None
        while node.getblockcount() < height:
            now = node.getblockcount()
            step = STAKE_CLOCK_STEP if height - now > 1 else LAST_BLOCK_CLOCK_STEP
            budget = STAKE_TIMEOUT * self.options.timeout_factor * (STAKE_CLOCK_STEP // step)
            if now != last_progress:
                last_progress = now
                deadline = time.time() + budget
            assert time.time() < deadline, "no block above %d in %d s; getstakinginfo: %s" % (now, budget, self.staking_info(node))
            self.advance_clock(step)
            poll_until = time.time() + STAKE_CLOCK_SLEEP
            while time.time() < poll_until and node.getblockcount() < height:
                time.sleep(0.2)
        assert_equal(node.setstaking(self.wallet_id), False)
        assert_equal(self.settled_height(node), height)
        return node.getblockhash(height)

    # ---- reading and forging a staked block ---------------------------------

    def coinbase_payouts(self, node, block_hash):
        """The paying outputs of a staked block's coinbase."""
        block = node.getblock(block_hash, 2)
        assert_equal(block["flags"], "proof-of-stake")
        return [out["value"] for out in block["tx"][0]["vout"] if out["value"] > 0]

    def staker_key(self, node, block):
        script = list(CScript(block.vtx[1].vout[1].scriptPubKey))
        assert_equal(len(script), 2)
        assert_equal(script[1], OP_CHECKSIG)
        pubkey = script[0]
        secret, _ = base58_to_byte(node.dumpprivkey(key_to_p2pkh(pubkey)))
        key = ECKey()
        key.set(secret[:32], True)
        assert_equal(key.get_pubkey().get_bytes(), pubkey)
        return key

    def forge(self, node, block_hash, extra=None):
        """The staker's block with an output added to its coinbase, signed again by the staker: a payout
        to the staker, or with no amount a data output of no value."""
        block = from_hex(CBlock(), node.getblock(block_hash, 0))
        assert block.is_proof_of_stake()
        assert_equal([out for out in block.vtx[0].vout if out.nValue > 0], [])
        if extra is None:
            block.vtx[0].vout.append(CTxOut(0, CScript([OP_RETURN, b"forged"])))
        else:
            block.vtx[0].vout.append(CTxOut(int(extra * COIN), block.vtx[1].vout[1].scriptPubKey))
        block.vtx[0].rehash()
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.vchBlockSig = self.staker_key(node, block).sign_ecdsa(ser_uint256(block.sha256))
        return block

    # ---- the test -----------------------------------------------------------

    def run_test(self):
        staker, old, masternode = self.nodes
        mn = self.mninfo[0]
        assert_equal(mn.node.index, masternode.index)
        self.wait_for_sporks_same()
        for node in self.nodes:
            force_finish_mnsync(node)
        for utxo in staker.listunspent(0):
            if utxo["amount"] == MASTERNODE_COLLATERAL:
                staker.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])

        self.log.info("Proof of work up to %d", LAST_POW_BLOCK)
        while staker.getblockcount() < LAST_POW_BLOCK:
            step = min(MINE_CHUNK, LAST_POW_BLOCK - staker.getblockcount())
            self.bump_mocktime(max(1, step // 5))
            self.generate(staker, step, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))
        for node in self.nodes:
            force_finish_mnsync(node)
        target = staker.getblockheader(staker.getbestblockhash())["time"] + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        while self.mocktime < target:
            self.mocktime = min(target, self.mocktime + 120)
            set_node_times(self.nodes, self.mocktime)
            time.sleep(0.2)

        self.log.info("Staked blocks pay the masternode, then it is revoked: registered, not payable")
        paid = self.stake_exactly_to(staker, LAST_POW_BLOCK + 2)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        assert_equal(self.coinbase_payouts(staker, paid), [Decimal("10000")])
        # A transaction waits for an InstantSend lock that no quorum will give before a block may take it.
        funds = staker.getnewaddress()
        fund = staker.sendtoaddress(funds, 1)
        self.advance_clock(TX_AGE)
        self.stake_exactly_to(staker, LAST_POW_BLOCK + 3)
        assert fund in staker.getblock(staker.getbestblockhash())["tx"]
        revoke = staker.protx("revoke", mn.proTxHash, mn.keyOperator, 1, funds)
        self.advance_clock(TX_AGE)
        self.stake_exactly_to(staker, LAST_POW_BLOCK + 4)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        assert revoke in staker.getblock(staker.getbestblockhash())["tx"]
        for node in self.nodes:
            state = node.protx("info", mn.proTxHash)["state"]
            assert state["PoSeBanHeight"] != -1, state
            assert_equal(list(node.masternodelist("status").values()), ["POSE_BANNED"])

        self.log.info("From here every honest block has an empty coinbase")
        self.stake_exactly_to(staker, H - 2)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        for height in range(LAST_POW_BLOCK + 5, H - 1):
            assert_equal(self.coinbase_payouts(staker, staker.getblockhash(height)), [])

        self.isolate_node(staker.index)

        self.log.info("H - 1: a copy paying %s extra is taken by the node with the height: the gap, below H", EXTRA)
        below = self.stake_exactly_to(staker, H - 1)
        assert_equal(self.coinbase_payouts(staker, below), [])
        below_raw = staker.getblock(below, 0)
        forged_below = self.forge(staker, below, EXTRA)
        assert_equal(masternode.submitblock(forged_below.serialize().hex()), None)
        assert_equal(masternode.getbestblockhash(), forged_below.hash)
        assert_equal(self.coinbase_payouts(masternode, forged_below.hash), [EXTRA])
        masternode.invalidateblock(forged_below.hash)
        no_value_below = self.forge(staker, below)
        assert_equal(masternode.submitblock(no_value_below.serialize().hex()), None)
        assert_equal(masternode.getbestblockhash(), no_value_below.hash)
        assert_equal(self.coinbase_payouts(masternode, no_value_below.hash), [])
        masternode.invalidateblock(no_value_below.hash)
        assert_equal(masternode.submitblock(below_raw), None)
        assert_equal(masternode.getbestblockhash(), below)
        assert_equal(old.submitblock(below_raw), None)
        assert_equal(old.getbestblockhash(), below)

        self.log.info("H: the same copy is refused by the node with the height, and taken by the node without it")
        at = self.stake_exactly_to(staker, H)
        assert_equal(self.coinbase_payouts(staker, at), [])
        at_raw = staker.getblock(at, 0)
        forged = self.forge(staker, at, EXTRA)
        assert_equal(masternode.submitblock(forged.serialize().hex()), "bad-cb-amount")
        assert_equal(masternode.getbestblockhash(), below)
        no_value = self.forge(staker, at)
        assert_equal(masternode.submitblock(no_value.serialize().hex()), "bad-cb-amount")
        assert_equal(masternode.getbestblockhash(), below)
        assert_equal(old.submitblock(forged.serialize().hex()), None)
        assert_equal(old.getbestblockhash(), forged.hash)
        assert_equal(self.coinbase_payouts(old, forged.hash), [EXTRA])

        self.log.info("The honest block H is taken by the node with the height; the other keeps the copy")
        assert_equal(masternode.submitblock(at_raw), None)
        assert_equal(masternode.getbestblockhash(), at)
        # The honest block has the same work as the copy, so it is stored beside it. It may already
        # have come from the masternode's node, which is the second answer and the same outcome.
        assert old.submitblock(at_raw) in ("inconclusive", "duplicate")
        assert_equal(old.getbestblockhash(), forged.hash)

        self.log.info("H + 1 follows")
        staker.setnetworkactive(True)
        self.connect_nodes(staker.index, masternode.index)
        after = self.stake_exactly_to(staker, H + 1)
        self.sync_blocks([staker, masternode], timeout=SYNC_TIMEOUT)
        assert_equal(self.coinbase_payouts(masternode, after), [])

        self.log.info("The node without the height goes back to the honest chain, which is now longer")
        # The network may have brought the block already, so the answer does not matter; the tip does.
        old.submitblock(staker.getblock(after, 0))
        self.wait_until(lambda: old.getbestblockhash() == after, timeout=SYNC_TIMEOUT)

        self.log.info("-reindex revalidates every block across H")
        self.restart_node(staker.index, extra_args=self.extra_args[staker.index] + ["-reindex"])
        self.wait_until(lambda: staker.getblockcount() == H + 1, timeout=REINDEX_TIMEOUT)
        assert_equal(staker.getbestblockhash(), after)

        self.log.info("The height cannot be scheduled on its own: superblocks must be retired by then")
        self.stop_node(old.index)
        old.assert_start_raises_init_error(self.extra_args[old.index] + ["-testactivationheight=v231@%d" % H],
                                           "Error: v23.1 must not precede the retirement of superblocks")

        self.log.info("Nor together with the platform share, which regtest runs from 900 unless pushed out")
        without = [a for a in self.extra_args[old.index] if not a.startswith("-testactivationheight=mn_rr@")]
        old.assert_start_raises_init_error(without + ["-testactivationheight=v231@%d" % H, "-testactivationheight=superblocksretired@%d" % H],
                                           "Error: v23.1 cannot be scheduled together with mn_rr")


if __name__ == '__main__':
    V231NoPayeeTest().main()
