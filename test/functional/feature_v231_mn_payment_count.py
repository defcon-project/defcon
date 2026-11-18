#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""From the v23.1 height every expected masternode payment needs a coinbase output of its own.

A masternode whose operator takes half the payment, paid to the owner's own
address, expects two equal outputs. Below the height one output satisfied both,
so a staked block could pay half, or pay half and keep the other half within the
coinbase bound. The network's one payable masternode is set up that way: the
framework's masternode is revoked, and the staker's wallet registers one with a
50% operator reward and points the operator payout at the owner's payout address.
It runs no node: with one masternode no quorum forms, so nothing bans it. Two
nodes, both with the height:

  node 0  stakes, and is cut off from the other while it stakes H - 1 and H, so
          each of its blocks can be offered to it by hand, forged copies first
  node 1  the checker (the revoked masternode's node)

What it shows:

  1  H - 1: the honest block pays the masternode two equal outputs of 5000. A copy
     paying only one of them, and a copy paying one and keeping the other for the
     staker, are both taken by the checker: the old rule, kept below H. The keeping
     copy then stays the chain's block H - 1 on both nodes.
  2  H, staked on that copy: the honest block pays two outputs of 2500, and the
     checker refuses both copies with bad-cb-payee, then takes the honest block.
  3  H + 1 follows.
  4  -reindex on both nodes revalidates the chain across H, and block H - 1 is still
     the copy only the old rule accepts.

Each copy is re-signed with the staker's key, so its signature is valid. The key
is read with dumpprivkey, so this runs on a legacy wallet.

What it does not show: a real network, more than one staker, or the platform share
(mn_rr is not scheduled on any release network and is pushed out of reach here).
"""

import time
from decimal import Decimal

from test_framework.address import base58_to_byte, key_to_p2pkh
from test_framework.key import ECKey
from test_framework.messages import CBlock, CTxOut, from_hex, ser_uint256
from test_framework.script import CScript, OP_CHECKSIG
from test_framework.test_framework import DashTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    force_finish_mnsync,
    p2p_port,
    set_node_times,
    softfork_active,
)

LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
MASTERNODE_COLLATERAL = 1000
OLD_HALF = Decimal("5000")
NEW_HALF = Decimal("2500")
H = 5016
NOT_IN_THIS_RUN = 6000000
NEVER = 2**31 - 1              # the largest height stands for never, as an unset gate holds it

MINE_CHUNK = 200
SETUP_BELOW = LAST_POW_BLOCK - 20  # the masternodes change while blocks are still mined
SYNC_TIMEOUT = 300
REINDEX_TIMEOUT = 900
STAKE_CLOCK_STEP = 8
LAST_BLOCK_CLOCK_STEP = 2
STAKE_CLOCK_SLEEP = 2
STAKE_TIMEOUT = 180
TX_SAFE = 10 * 60 + 1          # a transaction older than this goes into a block without an InstantSend lock


class V231MasternodePaymentCountTest(DashTestFramework):
    def set_test_params(self):
        rules = ["-testactivationheight=v20@%d" % NOT_IN_THIS_RUN,
                 "-testactivationheight=mn_rr@%d" % NEVER,
                 "-testactivationheight=v231@%d" % H,
                 "-testactivationheight=superblocksretired@%d" % H,
                 "-peertimeout=999999999",
                 "-staking=0"]
        self.set_dash_test_params(2, 1, extra_args=[
            rules + ["-staking=1", "-txindex=1", "-debug=pos"],
            rules,
        ])

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()
        if self.options.descriptors:
            raise SkipTest("reads the staker's key with dumpprivkey, which needs a legacy wallet")

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

    def mine(self, node, count):
        self.bump_mocktime(TX_SAFE)
        self.generate(node, count, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))

    # ---- reading and forging a staked block ---------------------------------

    def coinbase_payouts(self, node, block_hash):
        """The paying outputs of a staked block's coinbase, as (amount, address)."""
        block = node.getblock(block_hash, 2)
        assert_equal(block["flags"], "proof-of-stake")
        payouts = []
        for out in block["tx"][0]["vout"]:
            if out["value"] > 0:
                spk = out["scriptPubKey"]
                payouts.append((out["value"], spk.get("address") or spk.get("addresses", [None])[0]))
        return payouts

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

    def forge(self, node, block_hash, keep):
        """The staker's block with the second of its two equal masternode outputs dropped, or with keep, paid to
        the staker instead; signed again by the staker."""
        block = from_hex(CBlock(), node.getblock(block_hash, 0))
        assert block.is_proof_of_stake()
        paying = [i for i, out in enumerate(block.vtx[0].vout) if out.nValue > 0]
        assert_equal(len(paying), 2)
        first, second = (block.vtx[0].vout[i] for i in paying)
        assert_equal((first.nValue, first.scriptPubKey), (second.nValue, second.scriptPubKey))
        if keep:
            block.vtx[0].vout[paying[1]] = CTxOut(second.nValue, block.vtx[1].vout[1].scriptPubKey)
        else:
            del block.vtx[0].vout[paying[1]]
        block.vtx[0].rehash()
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.vchBlockSig = self.staker_key(node, block).sign_ecdsa(ser_uint256(block.sha256))
        return block

    # ---- the test -----------------------------------------------------------

    def run_test(self):
        staker, checker = self.nodes
        mn = self.mninfo[0]
        assert_equal(mn.node.index, checker.index)
        self.wait_for_sporks_same()
        for node in self.nodes:
            force_finish_mnsync(node)
        for utxo in staker.listunspent(0):
            if utxo["amount"] == MASTERNODE_COLLATERAL:
                staker.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])

        self.log.info("Proof of work up to %d", SETUP_BELOW)
        while staker.getblockcount() < SETUP_BELOW:
            step = min(MINE_CHUNK, SETUP_BELOW - staker.getblockcount())
            self.bump_mocktime(max(1, step // 5))
            self.generate(staker, step, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))

        self.log.info("A masternode with a 50% operator reward, paid to the owner's payout address")
        # A special transaction pays only with the coins at its fund or fee source address, and register_fund takes
        # the payout address when it is given none: one address for each of the three, funded in one transaction so
        # that no send spends another's coin (feature_dip3_deterministicmns.py funds register_fund the same way).
        fund, fee_service, fee_revoke = staker.getnewaddress(), staker.getnewaddress(), staker.getnewaddress()
        staker.sendmany("", {fund: MASTERNODE_COLLATERAL + Decimal("0.001"), fee_service: 1, fee_revoke: 1})
        self.mine(staker, 1)
        payout = staker.getnewaddress()
        owner = staker.getnewaddress()
        v19 = softfork_active(staker, "v19")
        bls = staker.bls("generate") if v19 else staker.bls("generate", True)
        service = "127.0.0.1:%d" % p2p_port(7)
        split = staker.protx("register_fund" if v19 else "register_fund_legacy",
                             staker.getnewaddress(), service, owner, bls["public"], owner, "50", payout, fund)
        self.mine(staker, 1)
        info = staker.protx("info", split)
        assert_equal(info["operatorReward"], 50)
        # register_fund locks the collateral it creates; keep it locked so that no send or stake spends it
        collateral = {"txid": info["collateralHash"], "vout": info["collateralIndex"]}
        if collateral not in staker.listlockunspent():
            staker.lockunspent(False, [collateral])
        staker.protx("update_service", split, service, bls["secret"], payout, fee_service)
        self.mine(staker, 1)
        state = staker.protx("info", split)["state"]
        assert_equal((state["payoutAddress"], state["operatorPayoutAddress"]), (payout, payout))

        self.log.info("The framework's masternode is revoked: the new one is the only payee")
        staker.protx("revoke", mn.proTxHash, mn.keyOperator, 1, fee_revoke)
        self.mine(staker, 1)
        for node in self.nodes:
            assert node.protx("info", mn.proTxHash)["state"]["PoSeBanHeight"] != -1
            assert_equal(sorted(node.masternodelist("status").values()), ["ENABLED", "POSE_BANNED"])

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

        self.log.info("Staked blocks pay the masternode two equal outputs to one address")
        self.stake_exactly_to(staker, H - 2)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        for height in range(LAST_POW_BLOCK + 1, H - 1):
            assert_equal(self.coinbase_payouts(staker, staker.getblockhash(height)), [(OLD_HALF, payout)] * 2)

        self.isolate_node(staker.index)
        staker_address = None

        self.log.info("H - 1: a copy paying half, and a copy keeping half, are taken by the checker: the old rule")
        below = self.stake_exactly_to(staker, H - 1)
        assert_equal(self.coinbase_payouts(staker, below), [(OLD_HALF, payout)] * 2)
        half_below = self.forge(staker, below, keep=False)
        assert_equal(checker.submitblock(half_below.serialize().hex()), None)
        assert_equal(checker.getbestblockhash(), half_below.hash)
        assert_equal(self.coinbase_payouts(checker, half_below.hash), [(OLD_HALF, payout)])
        checker.invalidateblock(half_below.hash)
        keep_below = self.forge(staker, below, keep=True)
        assert_equal(checker.submitblock(keep_below.serialize().hex()), None)
        assert_equal(checker.getbestblockhash(), keep_below.hash)
        [(_, paid), (_, staker_address)] = self.coinbase_payouts(checker, keep_below.hash)
        assert_equal(paid, payout)
        assert staker_address != payout

        self.log.info("The keeping copy becomes the staker's block H - 1 too")
        staker.invalidateblock(below)
        assert_equal(staker.submitblock(keep_below.serialize().hex()), None)
        assert_equal(staker.getbestblockhash(), keep_below.hash)

        self.log.info("H: both copies are refused with bad-cb-payee; the honest block is taken")
        at = self.stake_exactly_to(staker, H)
        assert_equal(staker.getblockheader(at)["previousblockhash"], keep_below.hash)
        assert_equal(self.coinbase_payouts(staker, at), [(NEW_HALF, payout)] * 2)
        at_raw = staker.getblock(at, 0)
        for keep in (False, True):
            forged = self.forge(staker, at, keep=keep)
            assert_equal(checker.submitblock(forged.serialize().hex()), "bad-cb-payee")
            assert_equal(checker.getbestblockhash(), keep_below.hash)
        assert_equal(checker.submitblock(at_raw), None)
        assert_equal(checker.getbestblockhash(), at)

        self.log.info("H + 1 follows")
        staker.setnetworkactive(True)
        self.connect_nodes(staker.index, checker.index)
        after = self.stake_exactly_to(staker, H + 1)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        assert_equal(self.coinbase_payouts(checker, after), [(NEW_HALF, payout)] * 2)

        self.log.info("-reindex on both nodes revalidates the chain across H, with the keeping copy at H - 1")
        for node in self.nodes:
            self.restart_node(node.index, extra_args=self.extra_args[node.index] + ["-reindex"])
        for node in self.nodes:
            self.wait_until(lambda: node.getblockcount() == H + 1, timeout=REINDEX_TIMEOUT)
            assert_equal(node.getbestblockhash(), after)
            assert_equal(node.getblockhash(H - 1), keep_below.hash)
            assert_equal(self.coinbase_payouts(node, keep_below.hash), [(OLD_HALF, payout), (OLD_HALF, staker_address)])


if __name__ == '__main__':
    V231MasternodePaymentCountTest().main()
