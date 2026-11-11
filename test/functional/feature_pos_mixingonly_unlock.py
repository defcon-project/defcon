#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A wallet unlocked for mixing only stakes, and still refuses to send or to
give out its keys.

`walletpassphrase <passphrase> <timeout> true` unlocks a wallet for CoinJoin
only: the keys are in memory, but every RPC that sends, signs a message or
exports a key checks the strict lock (EnsureWalletIsUnlocked) and refuses.
Staking needs the keys for the coinstake and the block signature and nothing
else, so the minter and the staking switch accept that unlock too.

What it does, in order:

1. encrypts the staking wallet; locked, the minter passes over it as locked
   and the chain stays at the last proof-of-work block (on a descriptor wallet
   the staking switch itself refuses to go on); the switch is then off;
2. unlocks it for mixing only: staking goes on and the next block is staked,
   with a time after the unlock (so no attempt from the locked phase made it);
3. while so unlocked, sending, message signing and key export are refused
   (dumpprivkey and dumpwallet on a legacy wallet, listdescriptors with
   private keys on a descriptor wallet); `send`, which never asked for the
   strict lock, hands the transaction back unsigned instead of sending it;
4. while unlocked for mixing only, a passphrase change keeps that mode (staking goes on, sending and key export
   stay refused), and so does a change with a wrong old passphrase (before, the first left the wallet fully
   unlocked and the second locked it);
5. after a restart the wallet is locked again, and the minter stakes nothing.

getwalletinfo's unlock_mode is checked at each stage of a full run: absent before the encryption, then "locked" or
"mixing_staking_only". --case runs one part of step 4 (after steps 1 to 3) without those checks: the negative control
on a build without the change (with --legacy-wallet, whose locked wallet fails on `send`).
"""

import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    force_finish_mnsync,
    set_node_times,
)

# chainparams.cpp, CRegTestParams
LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60
STAKE_CLOCK_STEP = 64
STAKE_TIMEOUT = 300
LOCKED_STEPS = 10
# A staking (legacy) wallet while locked: minter passes to wait for, one per clock step,
# and the minter's lines for them (-debug=pos, pos/minter.cpp). It sleeps seconds between passes.
LOCKED_PASSES = 3
MINTER_PASS_MSG = b"PoSMiner - using wallet"  # wait_for_debug_log reads bytes
LOCKED_PASS_MSG = ", locked wallet."  # assert_debug_log reads text
MINTER_PASS_TIMEOUT = 30
HEIGHT_QUIET_SECONDS = 4
HEIGHT_SETTLE_TIMEOUT = 60

POW_CHUNK = 500
POW_CHUNK_CLOCK_STEP = 100

PASSPHRASE = "staking only"
NEW_PASSPHRASE = "still staking only"
UNLOCK_SECONDS = 100000
# RPC_WALLET_PASSPHRASE_INCORRECT
PASSPHRASE_INCORRECT = -14
PASSPHRASE_INCORRECT_MSG = "The wallet passphrase entered was incorrect"
CASES = ["change", "wrong"]
# EnsureWalletIsUnlocked(), wallet/rpcwallet.cpp
UNLOCK_NEEDED = -13
UNLOCK_NEEDED_MSG = "Please enter the wallet passphrase with walletpassphrase first"


class PosMixingOnlyUnlockTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--case", choices=CASES, default=None, help="run one part of step 4, without the mode checks")

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=1", "-debug=pos"]]

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

    def stake_to_height(self, node, target_height, timeout=STAKE_TIMEOUT):
        """Move the clock on until the chain reaches target_height. The block may
        be there already: the minter can stake as soon as the switch goes on,
        before the first read here."""
        deadline = time.time() + timeout * self.options.timeout_factor
        while time.time() < deadline:
            if node.getblockcount() >= target_height:
                return
            self.advance_clock(STAKE_CLOCK_STEP)
            time.sleep(1)
        raise AssertionError("no block staked at height %d within %ds" % (target_height, timeout))

    def assert_no_block_staked(self, node, wallet_id, height):
        """A locked wallet must not stake: the chain stays at `height`, which the
        caller knows, rather than at whatever it is when this starts. With the
        staking switch on (a legacy wallet), each clock step waits for the minter's
        next pass over the wallet, and the passes must have found it locked; then
        the switch goes off, so that no pass can fall between the last check here
        and the caller's unlock. With the switch refused (a descriptor wallet), the
        minter has no wallet to stake with."""
        assert_equal(node.getblockcount(), height)
        if self.staking_info(node, wallet_id)["staking"]:
            with node.assert_debug_log([LOCKED_PASS_MSG]):
                for _ in range(LOCKED_PASSES):
                    with node.wait_for_debug_log([MINTER_PASS_MSG], timeout=MINTER_PASS_TIMEOUT):
                        self.advance_clock(STAKE_CLOCK_STEP)
                    assert_equal(node.getblockcount(), height)
            assert_equal(node.setstaking(wallet_id), False)
        else:
            for _ in range(LOCKED_STEPS):
                self.advance_clock(STAKE_CLOCK_STEP)
                time.sleep(1)
                assert_equal(node.getblockcount(), height)
        assert_equal(self.settled_height(node), height)

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

    def wallet_id(self, node):
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        return int(next(iter(wallets)))

    def staking_info(self, node, wallet_id):
        return node.getstakinginfo()[str(wallet_id)]

    def switch_staking_on_locked(self, node, wallet_id):
        """On a locked wallet the switch behaves by wallet kind: a descriptor
        wallet cannot register its coinstake descriptors and stays off; a legacy
        wallet goes on and the minter skips it as locked."""
        if self.staking_info(node, wallet_id)["staking"]:
            return
        assert_equal(node.setstaking(wallet_id), not self.options.descriptors)
        assert_equal(self.staking_info(node, wallet_id)["staking"], not self.options.descriptors)

    def assert_refuses_keys_and_sending(self, node, address, change, mixing_only):
        assert_raises_rpc_error(UNLOCK_NEEDED, UNLOCK_NEEDED_MSG, node.sendtoaddress, address, 1)
        mempool_before = node.getmempoolinfo()["size"]
        # A change address of its own: a locked legacy wallet with an empty
        # keypool could not hand out a new one, and would fail before signing.
        send = lambda: node.send(outputs={address: 1}, options={"change_address": change})
        if mixing_only or self.options.descriptors:
            # Unlocked for mixing only the wallet must not sign; fully locked, a
            # descriptor wallet cannot. Either way the transaction comes back unsigned.
            sent = send()
            assert_equal(sent["complete"], False)
            assert "txid" not in sent and "psbt" in sent
        else:
            # Fully locked, a legacy HD wallet fails on the key lookup instead, as it
            # always has (the change does not touch this state).
            assert_raises_rpc_error(-1, "GetKey: DecryptHDChain failed", send)
        assert_equal(node.getmempoolinfo()["size"], mempool_before)
        assert_raises_rpc_error(UNLOCK_NEEDED, UNLOCK_NEEDED_MSG, node.signmessage, address, "staking only")
        if self.options.descriptors:
            assert_raises_rpc_error(UNLOCK_NEEDED, UNLOCK_NEEDED_MSG, node.listdescriptors, True)
        else:
            assert_raises_rpc_error(UNLOCK_NEEDED, UNLOCK_NEEDED_MSG, node.dumpprivkey, address)
            assert_raises_rpc_error(UNLOCK_NEEDED, UNLOCK_NEEDED_MSG, node.dumpwallet,
                                    os.path.join(self.options.tmpdir, "mixingonly.dump"))

    def assert_mode(self, node, mode):
        """getwalletinfo's unlock_mode (a full run only: the controls run on a build without it)."""
        if self.options.case is None:
            info = node.getwalletinfo()
            if mode is None:
                assert "unlock_mode" not in info
            else:
                assert_equal(info["unlock_mode"], mode)

    def stake_one(self, node, wallet_id):
        """The next block, staked by the wallet as it is unlocked now; returns the settled height."""
        height = node.getblockcount()
        assert_equal(node.setstaking(wallet_id), True)
        self.stake_to_height(node, height + 1)
        assert_equal(node.setstaking(wallet_id), False)
        return self.settled_height(node)

    def run_test(self):
        node = self.nodes[0]
        if self.mocktime == 0:
            self.mocktime = node.getblockheader(node.getbestblockhash())["time"]
            set_node_times(self.nodes, self.mocktime)
        address = node.getnewaddress()
        change = node.getrawchangeaddress()

        self.log.info("Mining to the proof-of-stake boundary (height %d)", LAST_POW_BLOCK)
        self.mine_to_boundary(node, address)
        force_finish_mnsync(node)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        self.mocktime = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        set_node_times(self.nodes, self.mocktime)
        wallet_id = self.wallet_id(node)
        self.wait_until(lambda: self.staking_info(node, wallet_id)["minter_running"], timeout=30)

        self.assert_mode(node, None)

        self.log.info("Encrypted and locked: no block")
        node.encryptwallet(PASSPHRASE)
        assert_equal(node.getwalletinfo()["unlocked_until"], 0)
        self.assert_mode(node, "locked")
        self.switch_staking_on_locked(node, wallet_id)
        self.assert_no_block_staked(node, wallet_id, LAST_POW_BLOCK)
        # A staked block carries the time the minter read at the start of the attempt that
        # made it (pos/minter.cpp, the search time, read before the lock check; pos/stake.cpp,
        # SignBlock). The clock stands still from here until the unlock has finished, so any
        # attempt that read it while the wallet was locked has a time at or before
        # locked_until, however late its block reaches the chain; the clock moves on a step
        # only after the unlock.
        locked_until = self.mocktime

        self.log.info("Unlocked for mixing only: staking goes on and stakes")
        node.walletpassphrase(PASSPHRASE, UNLOCK_SECONDS, True)
        self.advance_clock(STAKE_CLOCK_STEP)
        assert_greater_than(node.getwalletinfo()["unlocked_until"], 0)
        self.assert_mode(node, "mixing_staking_only")
        # The switch is off (gone off, or refused, while locked): nothing has staked since
        # the locked check, so the next block is staked unlocked for mixing only.
        assert_equal(self.staking_info(node, wallet_id)["staking"], False)
        assert_equal(node.getblockcount(), LAST_POW_BLOCK)
        assert_equal(node.setstaking(wallet_id), True)
        assert_equal(self.staking_info(node, wallet_id)["staking"], True)
        staked_height = LAST_POW_BLOCK + 1
        self.stake_to_height(node, staked_height)
        assert_equal(node.setstaking(wallet_id), False)
        height = self.settled_height(node)
        staked = node.getblock(node.getblockhash(staked_height))
        assert_greater_than(len(staked["tx"]), 1)  # coinbase and coinstake
        # Made by an attempt that read the clock after the unlock had finished, so checked the
        # lock after it too: not one left over from the locked phase.
        assert_greater_than(staked["time"], locked_until)

        self.log.info("Still unlocked for mixing only: sending, signing and key export are refused")
        self.assert_refuses_keys_and_sending(node, address, change, mixing_only=True)

        case = self.options.case
        if case in (None, "change"):
            self.log.info("A passphrase change while unlocked for mixing only keeps that mode")
            node.walletpassphrasechange(PASSPHRASE, NEW_PASSPHRASE)
            self.assert_refuses_keys_and_sending(node, address, change, mixing_only=True)
            height = self.stake_one(node, wallet_id)
            self.assert_mode(node, "mixing_staking_only")
        if case in (None, "wrong"):
            self.log.info("A change with a wrong old passphrase changes nothing, the mode included")
            assert_raises_rpc_error(PASSPHRASE_INCORRECT, PASSPHRASE_INCORRECT_MSG, node.walletpassphrasechange,
                                    "not the passphrase", "another passphrase")
            self.assert_refuses_keys_and_sending(node, address, change, mixing_only=True)
            height = self.stake_one(node, wallet_id)
            self.assert_mode(node, "mixing_staking_only")

        self.log.info("After a restart the wallet is locked again: no block")
        self.restart_node(0)
        set_node_times(self.nodes, self.mocktime)
        force_finish_mnsync(node)
        assert_equal(node.getwalletinfo()["unlocked_until"], 0)
        self.assert_mode(node, "locked")
        wallet_id = self.wallet_id(node)
        self.wait_until(lambda: self.staking_info(node, wallet_id)["minter_running"], timeout=30)
        self.switch_staking_on_locked(node, wallet_id)
        self.assert_no_block_staked(node, wallet_id, height)
        self.assert_refuses_keys_and_sending(node, address, change, mixing_only=False)


if __name__ == '__main__':
    PosMixingOnlyUnlockTest().main()
