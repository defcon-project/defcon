#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The v23.1 height on staked blocks: what a masternode is paid, and which block is refused.

Every block from 5001 on is staked, as on the live network. Three nodes:

  node 0  stakes, and is cut off from the others while it stakes H - 1 and H, so
          each of its blocks can be offered to them by hand, a forged copy first
  node 1  runs without the height: a binary from before the release
  node 2  the masternode, running with the height

What it shows:

  1  H - 1: the staked block pays the masternode 10000. A copy paying 5000 is
     refused by both nodes.
  2  H: the staked block pays 5000. A copy paying 10000 is refused by the node
     with the height, and that same block is taken by the node without it, so
     the amount is the only thing wrong with it. That node refuses the honest
     block: it forks off at H, as a node on an old binary must. A copy that keeps
     the required 5000 and adds a second 5000 is refused too.
  3  llmq_test_v17 leaves the enabled types when the next block is H, on the node
     told to retire it, and stays there on the other.
  4  The chain goes on at 5000, and -reindex revalidates every block across H.

Each copy is re-signed with the staker's key, so its signature is valid.

What it does not show: regtest registers neither llmq_400_60 nor llmq_400_85, so a
test profile stands in for them here; chainparams_v23_bundle_tests holds them at the
height on devnet parameters, and feature_llmq_formation_end.py shows a retirement on
the chain. No live network, one staker, and no superblock height in the range.
"""

import time
from decimal import Decimal

from test_framework.address import base58_to_byte, key_to_p2pkh
from test_framework.key import ECKey
from test_framework.messages import COIN, CBlock, CTxOut, from_hex, ser_uint256
from test_framework.script import CScript, OP_CHECKSIG
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync, set_node_times

LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
MASTERNODE_COLLATERAL = 1000
OLD_PAYMENT = Decimal("10000")
NEW_PAYMENT = Decimal("5000")
H = 5016                       # staked, and on llmq_test_v17's 24-block grid
RETIRED = "llmq_test_v17"
FORMS_ON = "llmq_test"
# Regtest turns these on at 900 and no release network does; mn_rr would take a
# platform share out of the very payment this reads.
NOT_IN_THIS_RUN = 6000000

MINE_CHUNK = 200
SYNC_TIMEOUT = 300
REINDEX_TIMEOUT = 900
# posTimestampMask leaves two search times per 8 seconds, so a step of 8 always
# offers a fresh one, and a step of 2 opens at most one: the last block cannot overrun.
STAKE_CLOCK_STEP = 8
LAST_BLOCK_CLOCK_STEP = 2
STAKE_CLOCK_SLEEP = 2
STAKE_TIMEOUT = 180            # wall clock for one block, scaled by --timeout-factor


class V231TransitionTest(DashTestFramework):
    def set_test_params(self):
        rules = ["-vbparams=testdummy:0:999999999999:0:10:8:6:5:0",
                 "-testactivationheight=v20@%d" % NOT_IN_THIS_RUN,
                 "-testactivationheight=mn_rr@%d" % NOT_IN_THIS_RUN,
                 # ShouldRunInactivityChecks reads the mock clock, which moves by hours here.
                 "-peertimeout=999999999",
                 "-staking=0"]
        height = ["-testactivationheight=v231@%d" % H,
                  "-llmqformationendheight=%s:%d" % (RETIRED, H)]
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
        assert_equal(node.setstaking(self.wallet_id), True)   # a toggle: True is "now on"
        self.wait_until(lambda: self.staking_info(node)["minter_running"], timeout=30)

    def settled_height(self, node, quiet=4, timeout=90):
        """The block count once it has stopped moving, with staking off and the clock frozen."""
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

    def paid_to_masternode(self, node, block_hash):
        """The one paying output of a staked block's coinbase, which carries only the masternode payment."""
        block = node.getblock(block_hash, 2)
        assert_equal(block["flags"], "proof-of-stake")
        paid = [out["value"] for out in block["tx"][0]["vout"] if out["value"] > 0]
        assert_equal(len(paid), 1)
        return paid[0]

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

    def forge(self, node, block_hash, pay, second=None):
        """The staker's block, paying the masternode another amount, or a second output, and signed again by the staker."""
        block = from_hex(CBlock(), node.getblock(block_hash, 0))
        assert block.is_proof_of_stake()
        paying = [out for out in block.vtx[0].vout if out.nValue > 0]
        assert_equal(len(paying), 1)
        paying[0].nValue = int(pay * COIN)
        if second is not None:
            block.vtx[0].vout.append(CTxOut(int(second * COIN), paying[0].scriptPubKey))
        block.vtx[0].rehash()
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.vchBlockSig = self.staker_key(node, block).sign_ecdsa(ser_uint256(block.sha256))
        return block

    def listed(self, node):
        return RETIRED in node.quorum("list")

    # ---- the test -----------------------------------------------------------

    def run_test(self):
        staker, old, masternode = self.nodes
        assert_equal(self.mninfo[0].node.index, masternode.index)
        self.wait_for_sporks_same()
        for node in self.nodes:
            force_finish_mnsync(node)

        self.log.info("Lock the collateral: a coinstake eating it would remove the masternode")
        for utxo in staker.listunspent(0):
            if utxo["amount"] == MASTERNODE_COLLATERAL:
                staker.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        assert_equal(len(staker.masternodelist("status")), 1)

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

        self.log.info("Staked blocks up to H - 2 = %d, all paying the old amount", H - 2)
        self.stake_exactly_to(staker, H - 2)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        assert_equal(self.paid_to_masternode(staker, staker.getblockhash(LAST_POW_BLOCK + 1)), OLD_PAYMENT)
        assert_equal(self.paid_to_masternode(staker, staker.getblockhash(H - 2)), OLD_PAYMENT)

        self.log.info("Before H both nodes list %s: the next block is H - 1", RETIRED)
        assert self.listed(masternode)
        assert self.listed(old)

        self.isolate_node(staker.index)

        self.log.info("H - 1: the honest block pays %s; a copy paying %s is refused by both", OLD_PAYMENT, NEW_PAYMENT)
        below = self.stake_exactly_to(staker, H - 1)
        assert_equal(self.paid_to_masternode(staker, below), OLD_PAYMENT)
        below_raw = staker.getblock(below, 0)
        forged = self.forge(staker, below, NEW_PAYMENT).serialize().hex()
        for node in (masternode, old):
            assert_equal(node.getblockcount(), H - 2)
            assert_equal(node.submitblock(forged), "bad-cb-payee")
            assert_equal(node.getblockcount(), H - 2)
            assert_equal(node.submitblock(below_raw), None)
            assert_equal(node.getbestblockhash(), below)

        self.log.info("With H next, %s leaves the types of the node told to retire it, and only of that node", RETIRED)
        assert not self.listed(masternode)
        assert FORMS_ON in masternode.quorum("list")
        assert self.listed(old)

        self.log.info("H: the honest block pays %s", NEW_PAYMENT)
        at = self.stake_exactly_to(staker, H)
        assert_equal(self.paid_to_masternode(staker, at), NEW_PAYMENT)
        at_raw = staker.getblock(at, 0)

        self.log.info("A copy paying %s is refused by the node with the height", OLD_PAYMENT)
        forged_block = self.forge(staker, at, OLD_PAYMENT)
        forged = forged_block.serialize().hex()
        assert_equal(masternode.submitblock(forged), "bad-cb-amount")
        assert_equal(masternode.getbestblockhash(), below)

        self.log.info("So is a copy that keeps the required %s and adds a second one: the required output is there, the total is not allowed", NEW_PAYMENT)
        two_outputs = self.forge(staker, at, NEW_PAYMENT, second=NEW_PAYMENT).serialize().hex()
        assert_equal(masternode.submitblock(two_outputs), "bad-cb-amount")
        assert_equal(masternode.getbestblockhash(), below)

        self.log.info("The node without the height refuses the honest block and takes that copy: the amount was all that was wrong")
        assert_equal(old.submitblock(at_raw), "bad-cb-payee")
        assert_equal(old.getbestblockhash(), below)
        assert_equal(old.submitblock(forged), None)
        assert_equal(old.getbestblockhash(), forged_block.hash)
        assert_equal(self.paid_to_masternode(old, forged_block.hash), OLD_PAYMENT)

        self.log.info("The node with the height takes the honest block, and the two chains have parted at H")
        assert_equal(masternode.submitblock(at_raw), None)
        assert_equal(masternode.getbestblockhash(), at)

        self.log.info("H + 1: staked on top of H, still %s, and the masternode follows", NEW_PAYMENT)
        staker.setnetworkactive(True)
        self.connect_nodes(staker.index, masternode.index)
        after = self.stake_exactly_to(staker, H + 1)
        self.sync_blocks([staker, masternode], timeout=SYNC_TIMEOUT)
        assert_equal(self.paid_to_masternode(masternode, after), NEW_PAYMENT)
        assert_equal(old.getbestblockhash(), forged_block.hash)

        self.log.info("-reindex revalidates every block across H and arrives at the same tip")
        self.restart_node(staker.index, extra_args=self.extra_args[staker.index] + ["-reindex"])
        self.wait_until(lambda: staker.getblockcount() == H + 1, timeout=REINDEX_TIMEOUT)
        assert_equal(staker.getbestblockhash(), after)
        assert_equal(self.paid_to_masternode(staker, below), OLD_PAYMENT)
        assert_equal(self.paid_to_masternode(staker, at), NEW_PAYMENT)


if __name__ == '__main__':
    V231TransitionTest().main()
