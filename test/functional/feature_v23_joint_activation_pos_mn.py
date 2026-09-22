#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The v23 bundle on ONE chain that both stakes and carries masternodes, on the release timeline.

The proof-of-stake rehearsal crossed the activation on a chain with no
masternodes; the quorum rehearsal crossed it on a masternode network whose every
block was mined. This run puts both on one chain and keeps the release's own
schedule: the Sentinel layer starts 576 blocks after H, as the release derives
it, and every Q60 quorum between H and that start forms on staked blocks.

66 daemons: 65 masternodes and one node that produces every block from 5001 on
by staking, as the live network does.

  1 .. 5000     proof-of-work; the masternodes register, and the four Q60 quorums
                of the formation lead form
  5001 .. 5015  staked, inside the ChainLock pause
  5016 = H      staked; every rule of the bundle turns on in this block
  5040 .. 5568  staked; one Q60 DKG every 24 blocks, each waited for phase by
                phase, so the active signing set turns over from the quorums
                formed on proof-of-work blocks to quorums formed on staked ones
  5592 = H+576  the Sentinel layer starts recording, beside that window's DKG
  5616          its first commitment, in a staked block
  then          the staking node re-validates the whole chain with -reindex and
                drives the network again

What it shows, in this order:

  1  staked blocks are produced and accepted either side of H, and every stake
     modifier matches the rule of its own side
  2  on the staked tip H-1, no ChainLock is recovered for the sampled heights of
     [H-120, H) on the sampled nodes, and none arrives within the wait; the
     best lock stays below the pause
  3  block H is ChainLocked by llmq_defcon, and by nothing else
  4  a transaction at H is InstantSend-locked by a Q60 quorum and mined in H+1
  5  a masternode payment inside the coinbase of a staked block, equal to an
     amount and split computed by the test from the registry, on H-1, H, H+1
     and the commitment block
  6  Q60 quorums keep forming on staked blocks: every one from 5040 to 5568 has
     60 valid members and its commitment is mined in a staked block; once four
     of them are active, ChainLocks and an InstantSend lock are signed by one of
     them, and ChainLocks are still signed near the Sentinel start
  7  the Sentinel layer starts at H+576 on every node, and its first commitment
     is mined at index 2 of a staked block, signed by a quorum formed on staked
     blocks
  8  no masternode carries a PoSe penalty at the end: every DKG of the run
     completed
  9  after -reindex the staking node holds the same tip, quorum list, locks and
     commitment, and its next block is accepted everywhere and ChainLocked

What this run does NOT show, said here rather than found later:

  - Refusal. Every block here is honest; nothing offers the new rules a block
    they must reject -- not an oversized coinbase, not a malformed staked block,
    not a bad Sentinel commitment. Proof 5 shows the ceiling ACCEPTS the block
    the network builds; that it rejects one above it is unit-pinned, not shown.
  - The accept half of the pause. Every node carries the bundle, so no CLSIG for
    a paused height is ever offered to anyone. Proof 2 is a bounded observation
    on a sample, not an exhaustive search.
  - Fees and supply. The fee rule is identical on both sides of H on regtest
    (nPosFeeBurnActivationHeight defaults to 0 here), and the supply change is
    recorded, not asserted.
  - The payee order. Which masternode a block pays is taken from the block and
    checked against the registry and the node's own recomputation; there is no
    independent oracle of the selection order.
  - A missed masternode. Every masternode is up, so the commitment's missed
    bitfield is empty.
  - The Sentinel start block itself. The start height is the node's own report
    and is asserted on every node; the step that stakes to it tolerates one
    block of overrun, as the DKG steps do, so a run may first look at H+577.
    Every overrun is listed in the closing log line.
  - More than one staker. One node produces every block from 5001 on, so no
    two stakers ever compete for a height.
  - Network time. 66 daemons share one machine and one mock clock.

Not in test_runner.py: it raises the framework's node ceiling, a parallel job
with a different ceiling gets a different port stride, and one run takes hours.
Run it alone: test/functional/feature_v23_joint_activation_pos_mn.py --timeout-factor=2
"""

import os
import struct
import time
from decimal import Decimal

# Read at import time by test_framework, so it has to be set before the import.
os.environ.setdefault("TEST_RUNNER_MAX_NODES", "160")

from test_framework.authproxy import JSONRPCException
from test_framework.messages import CTransaction, from_hex, hash256, ser_compact_size, ser_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync, set_node_times

# ---- Consensus::LLMQType and llmq/params.h -----------------------------------
Q60_TYPE = 7
Q60_NAME = "llmq_defcon"
Q60_SIZE = 60
Q60_THRESHOLD = 41
Q60_DKG_INTERVAL = 24
Q60_SIGNING_ACTIVE = 4
Q60_MINING_WINDOW_END = 18
LEGACY_TYPE = 100           # llmq_test, regtest's llmqTypeChainLocks below H
LEGACY_NAME = "llmq_test"

# ---- chainparams.cpp, CRegTestParams -----------------------------------------
LAST_POW_BLOCK = 5000
STAKE_MIN_AGE = 10 * 60          # stakeAgeRange[0]
POS_TIMESTAMP_MASK = 5
MN_PAYMENT = Decimal("10000")    # GetMasternodePayment(), a flat amount on this chain
MASTERNODE_COLLATERAL = 1000     # regularMnCollateral
MN_COUNT = 65

# ---- the one number, and everything forced by it -----------------------------
# H must be a multiple of the Q60 DKG interval and (H + 576) a multiple of the
# Sentinel epoch interval; both are 24. 24*208 = 4992 is at or below
# lastPowBlock, so 24*209 = 5016 is the lowest activation height that lands in
# the staked era -- which is the whole point of this layer.
H = 5016
FORMATION_LEAD = (Q60_SIGNING_ACTIVE + 1) * Q60_DKG_INTERVAL   # 120
LEAD_START = H - FORMATION_LEAD                                # 4896
FIRST_ENABLED_TIP = LEAD_START - 1                             # 4895

EPOCH = 24
SENTINELS = 7                    # nDSLSentinelCount
CUTOFF = EPOCH - EPOCH // 4      # 18
SIGNING = EPOCH - EPOCH // 8     # 21
DSL_TX_TYPE = 10
UNREACHABLE = 2147483647

# The release's own derivation (chainparams.cpp, V23_DSL_ACTIVATION_OFFSET):
# no -testactivationheight=dsl@ here, so the node derives it from H exactly as a
# release network does, and the test reads it back from every node.
DSL_OFFSET = 24 * 24
DSL_START = H + DSL_OFFSET                # 5592
FIRST_EPOCH = DSL_START // EPOCH          # 233
FIRST_COMMITMENT = DSL_START + EPOCH      # 5616
FINAL_HEIGHT = FIRST_COMMITMENT + 1       # 5617

# The staked Q60 cycles: every base from the first after H+1 to the last before
# the Sentinel start; the window at DSL_START is walked beside the epoch.
STAKED_FIRST_BASE = H + Q60_DKG_INTERVAL                        # 5040
STAKED_LAST_BASE = DSL_START - Q60_DKG_INTERVAL                 # 5568
STAKED_BASES = list(range(STAKED_FIRST_BASE, STAKED_LAST_BASE + 1, Q60_DKG_INTERVAL))
# Where ChainLocks are checked against the quorums that formed on staked blocks:
# after the fourth staked quorum (the active set has turned over), and near the end.
CL_CHECK_AFTER = {STAKED_BASES[3], STAKED_BASES[len(STAKED_BASES) // 2], STAKED_BASES[-1]}

# Regtest activates v20 and mn_rr at 900. No release network does -- both sit at
# INT_MAX on mainnet, testnet and devnet. With mn_rr active GetBlockTxOuts
# prepends an OP_RETURN platform share to every coinbase, inside the exact
# transaction proof 5 reads, so it is pushed out of this chain's reach.
NOT_IN_THIS_RUN = 6000000

# ---- cuts, decided before the run rather than improvised during it -----------
Q60_ROUNDS = 4              # the formation lead's quorums, on proof-of-work blocks
NORMALISE_TIP = 4824        # 24*201, so mine_quorum's skip_count lands llmq_test on 4848
MINE_CHUNK = 200            # blocks per generatetoaddress call: well inside the RPC timeout under load

# The minter recomputes its search time from the mock clock on every attempt and
# refuses one that is not strictly after the tip's time; posTimestampMask clears
# bits 0 and 2, so only two search times exist per 8 seconds. A step of 8
# therefore always offers a fresh one. The mock:real ratio stays low on purpose:
# a signing session drops its shares after 60 MOCK seconds.
STAKE_CLOCK_STEP = 8
LAST_BLOCK_CLOCK_STEP = 2   # search times sit at 0 and 2 mod 8: a 2 s step opens at most one
STAKE_CLOCK_SLEEP = 2
STAKE_TIMEOUT = 180         # wall clock for ONE block, scaled by --timeout-factor

TX_AGE = 130                # WAIT_FOR_ISLOCK_TIMEOUT (120 s) plus a margin

CL_AT_H_TIMEOUT = 120
NO_CL_IN_PAUSE_WAIT = 30    # the handler retries from a 5 s task, so 30 not 15
ANNOUNCE_TIMEOUT = 180
VERDICT_TIMEOUT = 240
SIGN_TIMEOUT_PER_TICK = 40
SYNC_TIMEOUT = 300
REINDEX_TIMEOUT = 900


def modifier_of(prev_modifier, kernel_txid):
    """ComputeStakeModifier: Hash(kernel || previous modifier), each a uint256.

    RPC prints a uint256 byte-reversed, so both inputs are reversed back and the
    result reversed for printing. kernel_txid None is the rule below the gate:
    the kernel that was never filled in, 32 zero bytes.
    """
    kernel = bytes(32) if kernel_txid is None else bytes.fromhex(kernel_txid)[::-1]
    return hash256(kernel + bytes.fromhex(prev_modifier)[::-1])[::-1].hex()


def flip_last_bit(hex_hash):
    raw = bytearray(bytes.fromhex(hex_hash))
    raw[-1] ^= 1
    return bytes(raw).hex()


class V23JointActivationPosMnTest(DashTestFramework):
    def set_test_params(self):
        args = ["-testactivationheight=v23@%d" % H,
                "-testactivationheight=v20@%d" % NOT_IN_THIS_RUN,
                "-testactivationheight=mn_rr@%d" % NOT_IN_THIS_RUN,
                # ShouldRunInactivityChecks reads the MOCK clock, and this run
                # moves it by hours; the code's own comment names this flag.
                "-peertimeout=999999999"]
        # One independent list per node: `[args] * 66` would be 66 references to
        # the same list, and the staker-only arguments appended to node 0 below
        # would then reach every daemon.
        per_node = [list(args) for _ in range(66)]
        # node 0 stakes and is the only node with a wallet. -txindex is not a
        # convenience: `masternode payments` loops every non-coinbase
        # transaction of the block and dereferences GetTransaction with no null
        # check, and on a staked block the coinstake always enters that loop.
        per_node[0] += ["-staking=1", "-txindex=1", "-debug=pos"]
        self.set_dash_test_params(66, MN_COUNT, extra_args=per_node)
        # set AFTER set_dash_test_params, which resets them to llmq_test values
        self.llmq_size = Q60_SIZE
        self.llmq_threshold = Q60_THRESHOLD
        # 66 daemons load this machine to a CPU pressure of ~57 % on their own.
        # __init__ multiplies this by --timeout-factor after set_test_params returns.
        self.rpc_timeout = 240

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ---- small helpers ---------------------------------------------------

    def mark(self, text):
        self.log.info("[%4d s, height %d] %s", time.time() - self.started,
                      self.nodes[0].getblockcount(), text)

    def all_answer(self, what, predicate, timeout):
        """Wait until the predicate has held on every node; return what each answered."""
        pending = list(self.nodes)
        answers = {}
        deadline = time.time() + timeout * self.options.timeout_factor
        while True:
            still = []
            for n in pending:
                answer = predicate(n)
                if answer:
                    answers[n.index] = answer
                else:
                    still.append(n)
            pending = still
            if not pending:
                return answers
            assert time.time() < deadline, "%s: %d node(s) not there after %d s, the first is node%d" % (
                what, len(pending), timeout, pending[0].index)
            time.sleep(1)

    def mine_to(self, node, target, chunk=MINE_CHUNK, sync_fun=None):
        """Mine in chunks, keeping the mock clock ahead of the block times."""
        while node.getblockcount() < target:
            step = min(chunk, target - node.getblockcount())
            self.bump_mocktime(max(1, step // 5))
            self.generate(node, step, sync_fun=sync_fun or self.no_op)
        assert_equal(node.getblockcount(), target)

    def set_dkg(self, enabled):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0 if enabled else 4070908800)
        self.wait_for_sporks_same()

    # ---- staking ---------------------------------------------------------

    def staking_info(self, node):
        return node.getstakinginfo()[str(self.wallet_id)]

    def enable_staking(self, node):
        wallets = node.liststakingwallets()
        assert_greater_than(len(wallets), 0)
        self.wallet_id = int(next(iter(wallets)))
        assert_equal(node.setstaking(self.wallet_id), True)   # a toggle: True is "now on"
        self.wait_until(lambda: self.staking_info(node)["minter_running"], timeout=30)

    def disable_staking(self, node):
        assert_equal(node.setstaking(self.wallet_id), False)

    def advance_clock(self, seconds):
        self.mocktime += seconds
        set_node_times(self.nodes, self.mocktime)

    def settled_height(self, node, quiet=4, timeout=90):
        """The block count once it has stopped moving.

        With staking off and the clock frozen the minter cannot find a usable
        search time, so at most one more block can arrive -- an attempt that had
        already started. Measured rather than assumed.
        """
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

    def stake_exactly_to(self, node, height, exact=True):
        """Stake until the chain holds `height`, then switch staking off and prove not one more arrived.

        With exact (the default) an overrun is a failure, never a re-addressed
        target: every transaction and proof around H and in the Sentinel epoch
        is placed by the height it lands at. With exact=False one block past
        the target is accepted and counted: the DKG phase steps and the wait for
        a commitment only need to be inside a two-block phase or the mining
        window, and one more block cannot move them out of it.

        Run 1 stopped on a one-block overrun: the clock was stepped 8 s, which
        opens two stake search times, and the loop then slept 2 s blind, time
        enough for the minter to use both. For the last block the clock now
        moves 2 s, which opens at most one, and the loop polls instead of
        sleeping, so staking is switched off as soon as the target lands.
        """
        assert node.getblockcount() <= height, (node.getblockcount(), height)
        if node.getblockcount() == height:
            return node.getblockhash(height)
        self.enable_staking(node)
        last_progress = deadline = None
        while node.getblockcount() < height:
            now = node.getblockcount()
            if now != last_progress:
                last_progress = now
                deadline = time.time() + STAKE_TIMEOUT * self.options.timeout_factor
                if now % 50 == 0:
                    self.mark("staking towards %d" % height)
            assert time.time() < deadline, "no block above %d in %d s; getstakinginfo: %s; debug.log tail: %s" % (
                now, STAKE_TIMEOUT, self.staking_info(node), self.tail_debug_log(node))
            self.advance_clock(STAKE_CLOCK_STEP if height - now > 1 else LAST_BLOCK_CLOCK_STEP)
            poll_until = time.time() + STAKE_CLOCK_SLEEP
            while time.time() < poll_until and node.getblockcount() < height:
                time.sleep(0.2)
        self.disable_staking(node)
        settled = self.settled_height(node)
        if settled != height:
            assert not exact and settled == height + 1, "staked to %d, wanted %d%s" % (
                settled, height, "" if exact else " (at most one over)")
            self.overruns.append((height, settled))
            self.log.info("  one block over the target %d (%d); allowed at this step", height, settled)
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        return node.getblockhash(height)

    def tail_debug_log(self, node, lines=6):
        try:
            path = os.path.join(node.datadir, "regtest", "debug.log")
            with open(path, encoding="utf-8", errors="replace") as fh:
                return " | ".join(line.strip() for line in fh.readlines()[-lines:])
        except Exception as exc:                                    # pragma: no cover
            return "(could not read debug.log: %s)" % exc

    # ---- transactions ----------------------------------------------------

    def send_fee_tx(self, node, fee):
        """A transaction with a known fee, from a mature output the wallet is told not to stake.

        The wallet is never left to pick its own inputs here: it stakes, and
        coin selection has been seen to choose outputs that are not spendable,
        which returns a txid and never reaches the mempool.
        """
        utxo = node.listunspent(300, 9999999, [], True, {"maximumCount": 1})[0]
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        raw = node.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}],
                                        {node.getnewaddress(): utxo["amount"] - fee})
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"]
        txid = node.sendrawtransaction(signed["hex"])
        assert txid in node.getrawmempool(), "accepted but not in the mempool"
        assert_equal(Decimal(str(node.getmempoolentry(txid)["fees"]["base"])), fee)
        return txid

    def supply(self, node):
        return Decimal(str(node.gettxoutsetinfo()["total_amount"]))

    # ---- ChainLocks -------------------------------------------------------

    def wait_for_chainlock(self, node, block_hash, expected=True, timeout=CL_AT_H_TIMEOUT):
        def locked():
            self.bump_mocktime(1)
            try:
                block = node.getblock(block_hash)
                return block["confirmations"] > 0 and block["chainlock"]
            except Exception:
                return False
        return self.wait_until(locked, timeout=timeout, sleep=1, do_assert=expected)

    def best_chainlock(self, node):
        try:
            return node.getbestchainlock()
        except Exception:      # "Unable to find any ChainLock"
            return None

    def clsig_request_id(self, height):
        """SerializeHash(("clsig", int32 height)), as llmq/chainlocks.cpp builds it."""
        return hash256(ser_string(b"clsig") + struct.pack("<i", height))[::-1].hex()

    def signers_of(self, node, llmq_type, type_name, request_id, msg_hash, signature):
        """The quorums of one profile that accept this signature for this request.

        Asked quorum by quorum with the quorum named, so nothing depends on
        which one a height would select.
        """
        return [q for q in node.quorum("list", 100).get(type_name, [])
                if node.quorum("verify", llmq_type, request_id, msg_hash, signature, q)]

    def assert_lock_profile(self, node, lock, expect_q60):
        """Which profile SIGNED the lock -- getbestchainlock's llmqType only says which one the node expects."""
        request_id = self.clsig_request_id(lock["height"])
        q60 = self.signers_of(node, Q60_TYPE, Q60_NAME, request_id, lock["blockhash"], lock["signature"])
        legacy = self.signers_of(node, LEGACY_TYPE, LEGACY_NAME, request_id, lock["blockhash"], lock["signature"])
        self.log.info("  lock at %d: accepted by %d %s and %d %s quorum(s)",
                      lock["height"], len(q60), Q60_NAME, len(legacy), LEGACY_NAME)
        assert_equal((len(q60), len(legacy)), (1, 0) if expect_q60 else (0, 1))
        return q60

    def assert_tip_locked_by_staked_quorum(self, node, what):
        """The tip is ChainLocked, and the one quorum that signed it formed on staked blocks."""
        tip = node.getbestblockhash()
        self.wait_for_chainlock(node, tip)
        lock = node.getbestchainlock()
        assert_equal((lock["blockhash"], lock["llmqType"]), (tip, Q60_NAME))
        [signer] = self.assert_lock_profile(node, lock, expect_q60=True)
        formed_on_staked = set(self.staked_quorums) | ({self.epoch_quorum} if self.epoch_quorum else set())
        assert signer in formed_on_staked, "%s: the lock at %d was signed by %s, not a quorum formed on staked blocks" % (
            what, lock["height"], signer)
        return lock

    # ---- InstantSend ------------------------------------------------------

    def islock_request_id(self, node, txid):
        tx = from_hex(CTransaction(), node.getrawtransaction(txid))
        buf = ser_string(b"islock") + ser_compact_size(len(tx.vin))
        for txin in tx.vin:
            buf += txin.prevout.serialize()
        return hash256(buf)[::-1].hex()

    def q60_signed(self, node, txid, timeout=0):
        request_id = self.islock_request_id(node, txid)

        def signed():
            self.bump_mocktime(1)
            try:
                return node.quorum("hasrecsig", Q60_TYPE, request_id, txid)
            except Exception:
                return False
        if timeout == 0:
            return signed()
        return self.wait_until(signed, timeout=timeout, sleep=1, do_assert=False)

    def wait_for_internal_islock(self, node, txid, timeout=60):
        """instantlock_internal, never instantlock: the plain field is islock OR chainlock."""
        def locked():
            self.bump_mocktime(1)
            try:
                return node.getrawtransaction(txid, True)["instantlock_internal"]
            except Exception:
                return False
        self.wait_until(locked, timeout=timeout, sleep=1)

    # ---- the Sentinel layer ----------------------------------------------

    def assert_sentinel_dormant(self):
        for n in self.nodes:
            dsl = n.dslstatus()
            assert_equal(dsl["activationheight"], DSL_START)
            assert_equal(dsl["enforcementheight"], UNREACHABLE)
            assert_equal((dsl["active"], dsl["enforcing"]), (False, False))
            assert_equal((dsl["respondedcount"], dsl["epochreports"]), (0, 0))

    def dsl_request_id(self, node, epoch):
        base = bytes.fromhex(node.getblockhash(epoch * EPOCH))[::-1]
        return hash256(ser_string(b"dslcommitment") + struct.pack("<I", epoch) + base)[::-1].hex()

    def dsl_signature_recovered(self, node, epoch):
        self.bump_mocktime(1)
        request_id = self.dsl_request_id(node, epoch)
        return (node.quorum("isconflicting", Q60_TYPE, request_id, "00" * 32) or
                node.quorum("isconflicting", Q60_TYPE, request_id, "11" * 32))

    def commitment_at(self, node, height):
        """The service commitment a block carries: (json, msg_hash, signature)."""
        block_hash = node.getblockhash(height)
        block = node.getblock(block_hash, 2)
        txs = [tx for tx in block["tx"] if tx.get("type") == DSL_TX_TYPE]
        assert_equal(len(txs), 1)
        raw = txs[0].get("hex") or node.getrawtransaction(txs[0]["txid"], False, block_hash)
        tx = from_hex(CTransaction(), raw)
        assert_equal(tx.nType, DSL_TX_TYPE)
        signature = tx.vExtraPayload[-96:]
        tx.vExtraPayload = tx.vExtraPayload[:-96] + bytes(96)
        msg_hash = hash256(tx.serialize())[::-1].hex()
        return txs[0]["poseServiceTx"]["commitment"], msg_hash, signature.hex()

    # ---- quorum formation -------------------------------------------------

    def wait_for_quorum_connections(self, quorum_hash, expected_connections, mninfos,
                                    llmq_type_name="llmq_test", timeout=60, wait_proc=None):
        """For llmq_defcon: all sixty members hold a session, and every one's quorum connections are up.

        The framework's version answers at the first masternode it finds with a
        session and enough connections, which is all a three-member quorum
        needs; a member with no session or an empty connection list passes it.
        Here a member with no session is not counted, and the wait ends only
        when sixty members are counted.
        """
        if llmq_type_name != Q60_NAME:
            return super().wait_for_quorum_connections(quorum_hash, expected_connections, mninfos,
                                                       llmq_type_name=llmq_type_name,
                                                       timeout=timeout, wait_proc=wait_proc)

        def sixty_members_connected():
            members = 0
            for mn in mninfos:
                status = mn.node.quorum("dkgstatus")
                if not any(s["llmqType"] == Q60_NAME and s["status"]["quorumHash"] == quorum_hash
                           for s in status["session"]):
                    continue
                conns = [qc for qc in status["quorumConnections"]
                         if qc["llmqType"] == Q60_NAME and qc["quorumHash"] == quorum_hash]
                links = conns[0].get("quorumConnections", []) if conns else []
                if len(links) < expected_connections or not all(c["connected"] for c in links):
                    if wait_proc is not None:
                        wait_proc()
                    return False
                members += 1
            return members == Q60_SIZE
        self.wait_until(sixty_members_connected, timeout=timeout, sleep=1)

    def form_q60(self, node, expected_base):
        """A lead quorum, on proof-of-work blocks (mine_quorum mines them)."""
        quorum_hash = self.mine_quorum(llmq_type_name=Q60_NAME, llmq_type=Q60_TYPE,
                                       expected_members=Q60_SIZE,
                                       expected_contributions=Q60_SIZE,
                                       expected_commitments=Q60_SIZE)
        assert_equal(node.quorum("info", Q60_TYPE, quorum_hash)["height"], expected_base)
        return quorum_hash

    def q60_connections(self, node):
        """As mine_quorum reads them: with SPORK_21 off a member is asked for two quorum connections."""
        sporks = node.spork("show")
        # probes (SPORK_23) are not waited for below, so they must be off, as in the lead rounds
        assert sporks["SPORK_23_QUORUM_POSE"] > 1
        return (Q60_SIZE - 1) if sporks["SPORK_21_QUORUM_ALL_CONNECTED"] <= 1 else 2

    def dkg_phases(self, node, base, between=None):
        """Phases 2..6 of the DKG based at `base`, each after staking the two blocks it needs.

        `between` runs after phase 1, while the chain stands at the base.
        """
        mns = self.mninfo
        base_height = node.getblock(base)["height"]
        assert node.getblockcount() <= base_height + 1   # phase 1 is the base and the block after it
        self.wait_for_quorum_phase(base, 1, Q60_SIZE, None, 0, mns, llmq_type_name=Q60_NAME)
        self.wait_for_quorum_connections(base, self.q60_connections(node), mns,
                                         wait_proc=lambda: self.bump_mocktime(1), llmq_type_name=Q60_NAME)
        if between is not None:
            between()
        for phase, field, count in ((2, "receivedContributions", Q60_SIZE),
                                    (3, "receivedComplaints", 0),
                                    (4, "receivedJustifications", 0),
                                    (5, "receivedPrematureCommitments", Q60_SIZE),
                                    (6, None, 0)):
            # Phase n is the two blocks from base + 2(n-1). The target is absolute, so a one-block
            # overrun stays inside the same phase and can never carry a later step past one.
            target = base_height + 2 * (phase - 1)
            if node.getblockcount() < target:
                self.stake_exactly_to(node, target, exact=False)
            assert node.getblockcount() <= target + 1, ("phase %d overrun" % phase, node.getblockcount(), target)
            self.wait_for_quorum_phase(base, phase, Q60_SIZE, field, count, mns, llmq_type_name=Q60_NAME)
        self.wait_for_quorum_commitment(base, self.nodes, llmq_type=Q60_TYPE)
        # The commitment is mined in the first block of the window that carries it;
        # stake one block at a time, never past the window.
        while base not in node.quorum("list").get(Q60_NAME, []):
            assert node.getblockcount() < base_height + Q60_MINING_WINDOW_END, \
                "the commitment for %d was not mined inside its window" % base_height
            self.bump_mocktime(1)
            self.stake_exactly_to(node, node.getblockcount() + 1, exact=False)
        info = node.quorum("info", Q60_TYPE, base)
        assert_equal(info["height"], base_height)
        valid = sum(1 for m in info["members"] if m["valid"])
        assert_equal((len(info["members"]), valid), (Q60_SIZE, Q60_SIZE))
        mined = node.getblock(info["minedBlock"])
        assert_equal(mined["flags"], "proof-of-stake")
        return mined["height"]

    def stake_q60(self, node, base_height):
        """One Q60 cycle on staked blocks, every phase waited for. Returns (quorum hash, mined height)."""
        assert node.getblockcount() < base_height, (node.getblockcount(), base_height)
        base = self.stake_exactly_to(node, base_height, exact=False)
        mined_at = self.dkg_phases(node, base)
        return base, mined_at

    # ---- the six proofs at H ------------------------------------------------

    def assert_pos_regime(self, node, height, prev_time=None):
        """Proof 1, per block: this really is a staked block, built the way the wallet builds one."""
        block = node.getblock(node.getblockhash(height), 2)
        assert_equal(block["flags"], "proof-of-stake")
        assert_equal(block["nonce"], 0)
        assert_equal(block["time"] & POS_TIMESTAMP_MASK, 0)
        if prev_time is not None:
            assert block["time"] > prev_time, (height, block["time"], prev_time)
        coinstake = block["tx"][1]
        assert_equal(coinstake["vout"][0]["value"], 0)
        assert_equal(coinstake["vout"][0]["scriptPubKey"]["hex"], "")
        assert_equal(coinstake["vout"][1]["scriptPubKey"]["type"], "pubkey")
        return block

    def assert_pow_refused_above_boundary(self, node, address):
        """Without this, merely reaching 5000 proves nothing about the regime."""
        before = node.getblockcount()
        try:
            self.generatetoaddress(node, 1, address, sync_fun=self.no_op)
        except JSONRPCException as e:
            message = e.error["message"]
            assert ("bad-pos-nonce" in message) or ("pow-late" in message), message
        else:
            raise AssertionError("a proof-of-work block was accepted above lastPowBlock")
        assert_equal(node.getblockcount(), before)

    def check_modifiers(self, node, first, last):
        """Proof 1's switch: recompute every staked block's modifier from the rule of its own side of H."""
        before = after = 0
        prev = node.getblock(node.getblockhash(first))["modifier"]
        for height in range(first + 1, last + 1):
            block = node.getblock(node.getblockhash(height), 2)
            assert_equal(block["flags"], "proof-of-stake")
            kernel = block["tx"][1]["vin"][0]["txid"] if height >= H else None
            assert_equal(block["modifier"], modifier_of(prev, kernel))
            if height >= H:
                after += 1
            else:
                before += 1
            prev = block["modifier"]
        return before, after

    def build_payee_registry(self, node):
        """Every masternode's payout and operator addresses, from the registry rather than from the block.

        `protx list registered true` reports ADDRESSES (state.payoutAddress,
        state.operatorPayoutAddress) and the operator reward as a percentage at
        the top level (evo/dmnstate.cpp, evo/deterministicmns.cpp:68). The
        framework gives every masternode its own payout address and, when its
        operator reward is non-zero, its own operator address -- the payee of a
        block can only be named from the block because of that, so it is asserted.
        """
        self.payee_by_address = {}
        self.payout_address = {}
        self.operator_address = {}
        self.operator_reward = {}
        for mn in node.protx("list", "registered", True):
            state = mn["state"]
            pro_tx_hash = mn["proTxHash"]
            self.payout_address[pro_tx_hash] = state["payoutAddress"]
            self.payee_by_address[state["payoutAddress"]] = pro_tx_hash
            self.operator_reward[pro_tx_hash] = Decimal(str(mn.get("operatorReward", 0)))
            operator = state.get("operatorPayoutAddress", "")
            if operator:
                self.operator_address[pro_tx_hash] = operator
                self.payee_by_address[operator] = pro_tx_hash
        assert_equal(len(self.operator_reward), MN_COUNT)
        assert_equal(len(set(self.payout_address.values())), MN_COUNT)   # one payout address per masternode
        self.log.info("Registry: %d masternodes, %d distinct payout addresses, %d with an operator reward",
                      MN_COUNT, MN_COUNT, sum(1 for r in self.operator_reward.values() if r > 0))

    def assert_mn_payment_in_pos_coinbase(self, node, height):
        """Proof 5: the masternode payment in a staked block's coinbase equals what the test computes.

        The amount is the flat regtest payment and the split is computed here
        from the registry, not taken from the function under test:
        GetBlockTxOuts divides the payment between the payout and the
        operator-payout script whenever the payee carries an operator reward,
        and the framework gives every masternode but index 0 one. This shows the
        honest block is accepted with exactly this payment; it does not show
        that a larger one is refused.
        """
        block = node.getblock(node.getblockhash(height), 2)
        assert_equal(block["flags"], "proof-of-stake")
        coinbase = block["tx"][0]
        assert_equal(coinbase["vout"][0]["value"], 0)
        assert_equal(coinbase["vout"][0]["scriptPubKey"]["hex"], "")
        outs = coinbase["vout"][1:]
        assert 1 <= len(outs) <= 2, (height, len(outs))
        # mn_rr really is out of reach: no OP_RETURN platform share
        assert all(o["scriptPubKey"]["type"] != "nulldata" for o in outs), outs
        assert_equal(sum(o["value"] for o in outs), MN_PAYMENT)

        # the payee, named from the block's own outputs; both outputs must belong to ONE masternode
        owners = {self.payee_by_address.get(o["scriptPubKey"].get("address")) for o in outs}
        assert None not in owners, "an output pays an address no registered masternode owns: %s" % outs
        assert_equal(len(owners), 1)
        [pro_tx_hash] = owners
        # the split, computed from the registry and compared as a set, so the output order does not matter:
        # GetBlockTxOuts gives the operator (masternodeReward * nOperatorReward) / 10000, i.e. percent / 100
        reward = self.operator_reward[pro_tx_hash]
        operator_share = MN_PAYMENT * reward / 100
        expected = {(self.payout_address[pro_tx_hash], MN_PAYMENT - operator_share)}
        if reward > 0:
            expected.add((self.operator_address[pro_tx_hash], operator_share))
        got = {(o["scriptPubKey"]["address"], o["value"]) for o in outs}
        assert_equal(got, expected)

        # corroboration from the node's own recomputation. It re-runs
        # FillBlockPayments from the block's predecessor, so it is not an
        # independent judge of the payee order.
        [row] = node.masternode("payments", node.getblockhash(height), 1)
        want = {(p["script"], p["amount"]) for p in row["masternodes"][0]["payees"]}
        assert want, "GetBlockTxOuts returned nothing; the subset check would pass vacuously"
        paid = {(o["scriptPubKey"]["hex"], int(o["value"] * 100000000)) for o in outs}
        assert want <= paid, (want, paid)
        assert_equal(row["masternodes"][0]["proTxHash"], pro_tx_hash)
        return pro_tx_hash

    # ---- the first Sentinel epoch, beside that window's DKG ---------------

    def walk_first_epoch(self, node):
        """From DSL_START - 1 to FIRST_COMMITMENT - 1, every phase of both protocols waited for.

        The Sentinel epoch and the Q60 DKG cycle share a grid, on this chain and
        on the release networks, so the first epoch runs beside a DKG. Returns
        the quorum that DKG formed and the position the threshold signature was
        there at.
        """
        assert_equal(node.getblockcount(), DSL_START - 1)
        self.bump_mocktime(1)
        base = self.stake_exactly_to(node, DSL_START, exact=False)

        self.log.info("AT %d = H + %d: the Sentinel layer is active on every node, in epoch %d",
                      DSL_START, DSL_OFFSET, FIRST_EPOCH)

        def in_first_epoch(n):
            dsl = n.dslstatus()
            return dsl if dsl["epoch"] == FIRST_EPOCH else None
        for dsl in self.all_answer("the first epoch", in_first_epoch, 120).values():
            assert_equal((dsl["active"], dsl["enforcing"]), (True, False))
            assert_equal(dsl["epochblockhash"], base)

        def announcements():
            self.bump_mocktime(60)
            self.all_answer("the announcements", lambda n: n.dslstatus()["respondedcount"] == MN_COUNT,
                            ANNOUNCE_TIMEOUT)
            self.mark("epoch %d is live on every node and all %d masternodes announced" % (FIRST_EPOCH, MN_COUNT))

        mined_at = self.dkg_phases(node, base, between=announcements)
        assert node.getblockcount() < DSL_START + CUTOFF, "the DKG beside the epoch ran past its cutoff"
        # formed on staked blocks too; kept apart so the commitment check can tell it from the run-up's
        self.epoch_quorum = base
        self.mark("the DKG beside the first epoch formed its quorum, mined in staked block %d" % mined_at)

        self.log.info("The cutoff, +%d: reports, and the verdict they add up to on every node", CUTOFF)
        self.stake_exactly_to(node, DSL_START + CUTOFF, exact=False)
        reports = MN_COUNT * SENTINELS

        def pool_complete(n):
            dsl = n.dslstatus()
            return dsl if dsl["epochreports"] == reports else None
        pools = self.all_answer("the report pool", pool_complete, VERDICT_TIMEOUT)
        for dsl in pools.values():
            assert_equal((dsl["onlinereports"], dsl["missedreports"]), (reports, 0))
            candidate = dsl["candidate"]
            assert_equal((candidate["version"], candidate["missedcount"], candidate["unobservedcount"]), (2, 0, 0))
        assert_equal(len({dsl["poolhash"] for dsl in pools.values()}), 1)
        self.mark("%d reports, all online, one pool and one verdict on every node" % reports)

        self.log.info("The signing window, +%d..+%d", SIGNING, EPOCH - 1)
        self.stake_exactly_to(node, DSL_START + SIGNING)
        signed_at = None
        for pos in range(SIGNING, EPOCH):
            assert_equal(node.getblockcount(), DSL_START + pos)
            if self.wait_until(lambda: self.dsl_signature_recovered(node, FIRST_EPOCH),
                               timeout=SIGN_TIMEOUT_PER_TICK, sleep=1, do_assert=False):
                signed_at = pos
                break
            if pos < EPOCH - 1:
                self.stake_exactly_to(node, DSL_START + pos + 1)
        assert signed_at is not None, "no threshold signature for the first epoch by its last block"
        self.mark("the threshold signature was there at +%d" % signed_at)
        return signed_at

    # ---- the run ----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.started = time.time()
        self.overruns = []
        self.log.info("H = %d, Sentinel start = %d (H + %d), first commitment = %d, staked Q60 bases %d..%d (%d)",
                      H, DSL_START, DSL_OFFSET, FIRST_COMMITMENT, STAKED_FIRST_BASE, STAKED_LAST_BASE,
                      len(STAKED_BASES))
        assert_equal(H % Q60_DKG_INTERVAL, 0)
        assert_equal(DSL_START % EPOCH, 0)
        assert H > LAST_POW_BLOCK, "the activation must land in the staked era"

        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        # ---- the collaterals, before node 0 holds thousands of coins -------
        # The framework locks only the odd-index collaterals; the rest, and the
        # leftover 1000-DFCN outputs it created for every index, are legal stake
        # kernels on regtest. A coinstake eating one would remove a masternode
        # from the quorums this whole run depends on, and nothing would say so.
        locked = 0
        for utxo in node.listunspent(0):
            if utxo["amount"] == MASTERNODE_COLLATERAL:
                node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
                locked += 1
        self.log.info("Locked %d more output(s) of exactly %d DFCN; outputs now locked in total: %d",
                      locked, MASTERNODE_COLLATERAL, len(node.listlockunspent()))
        assert len(node.listlockunspent()) >= MN_COUNT, "fewer locked outputs than masternode collaterals"
        assert_equal(len(node.masternodelist("status")), MN_COUNT)

        self.build_payee_registry(node)
        # The Sentinel start is the node's own derivation from H: every node must report H + 576.
        self.assert_sentinel_dormant()
        self.mark("%d masternodes up, the Sentinel layer dormant on all %d nodes with its start at %d"
                  % (MN_COUNT, len(self.nodes), DSL_START))

        # ---- the proof-of-work era ----------------------------------------
        self.mine_to(node, NORMALISE_TIP, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))
        self.mark("normalised to %d" % NORMALISE_TIP)

        self.log.info("A ChainLock under the profile the chain uses below H")
        self.set_dkg(True)
        # llmq_test is 3 members (llmq/params.h); mine_quorum would otherwise wait for self.llmq_size
        self.mine_quorum(llmq_type_name=LEGACY_NAME, llmq_type=LEGACY_TYPE,
                         expected_members=3, expected_connections=2,
                         expected_contributions=3, expected_commitments=3)
        assert node.getblockcount() < FIRST_ENABLED_TIP, "the legacy quorum ran into the formation lead"
        self.bump_mocktime(1)
        legacy_tip = self.generate(node, 1, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))[0]
        self.wait_for_chainlock(node, legacy_tip)
        legacy_lock = node.getbestchainlock()
        assert legacy_lock["height"] < LEAD_START, (legacy_lock["height"], LEAD_START)
        assert_equal(legacy_lock["llmqType"], LEGACY_NAME)
        assert node.verifychainlock(legacy_lock["blockhash"], legacy_lock["signature"], legacy_lock["height"])
        self.assert_lock_profile(node, legacy_lock, expect_q60=False)
        self.mark("a %s ChainLock at %d, below the pause" % (LEGACY_NAME, legacy_lock["height"]))

        self.log.info("The Q60 profile is enabled for the block after %d and not before", FIRST_ENABLED_TIP)
        self.mine_to(node, FIRST_ENABLED_TIP - 1, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))
        assert Q60_NAME not in node.quorum("list"), node.quorum("list")
        self.mine_to(node, FIRST_ENABLED_TIP, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))
        assert Q60_NAME in node.quorum("list"), node.quorum("list")

        self.log.info("Forming %d Q60 quorums inside the formation lead, on proof-of-work blocks", Q60_ROUNDS)
        self.lead_quorums = []
        for i in range(Q60_ROUNDS):
            base_height = LEAD_START + i * Q60_DKG_INTERVAL
            self.lead_quorums.append(self.form_q60(node, base_height))
            self.mark("lead quorum %d of %d, base %d" % (i + 1, Q60_ROUNDS, base_height))
        # `quorum listextended` takes a HEIGHT, not a count, and answers the
        # signingActiveQuorumCount newest quorums as of that height -- so ask it about the tip.
        assert_equal(len(node.quorum("listextended")[Q60_NAME]), Q60_ROUNDS)

        # DKG off until after H + 1: from here to 5039 no cycle is waited for, and an
        # unpaced 60-member session can end in a partial commitment that punishes
        # members. Signing is not gated by this spork: the lead quorums go on signing.
        self.set_dkg(False)

        # ---- the handover to staking --------------------------------------
        self.mine_to(node, LAST_POW_BLOCK, sync_fun=lambda: self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT))
        address = node.getnewaddress()
        self.assert_pow_refused_above_boundary(node, address)
        self.mark("proof-of-work era finished at %d; a proof-of-work block above it is refused" % LAST_POW_BLOCK)

        for n in self.nodes:
            force_finish_mnsync(n)
        self.all_answer("the chain synced before staking",
                        lambda n: n.mnsync("status")["IsBlockchainSynced"], 120)
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        target = tip_time + STAKE_MIN_AGE + STAKE_CLOCK_STEP
        while self.mocktime < target:
            self.mocktime = min(target, self.mocktime + 120)
            set_node_times(self.nodes, self.mocktime)
            time.sleep(0.2)
        self.mark("clock moved past the stake age floor; staking may begin")

        # ---- staked blocks, before H --------------------------------------
        self.stake_exactly_to(node, LAST_POW_BLOCK + 1)
        self.assert_pos_regime(node, LAST_POW_BLOCK + 1)
        self.mark("the first staked block, %d" % (LAST_POW_BLOCK + 1))

        self.stake_exactly_to(node, H - 2)
        supply_before = self.supply(node)
        self.mark("staked to H - 2 = %d" % (H - 2))

        self.log.info("Block H - 1 carries a fee-paying transaction; no quorum can lock it below H")
        tx1 = self.send_fee_tx(node, Decimal("0.001"))
        self.sync_mempools(timeout=SYNC_TIMEOUT)
        self.advance_clock(TX_AGE)
        h1_hash = self.stake_exactly_to(node, H - 1)
        assert tx1 in node.getblock(h1_hash)["tx"], "the transaction was not mined in block H - 1"

        # ---- proof 2: the pause, on a staked tip ---------------------------
        self.log.info("The pause, observed: no ChainLock on H - 1 within the wait, and none recovered for the sample")
        assert_equal(node.getblockcount(), H - 1)
        assert not self.wait_for_chainlock(node, h1_hash, expected=False, timeout=NO_CL_IN_PAUSE_WAIT), \
            "a block inside the pause was ChainLocked"
        best = self.best_chainlock(node)
        assert best is not None and best["height"] < LEAD_START, best
        assert_equal(best["llmqType"], LEGACY_NAME)
        # the SIGNING half, asked directly of the local recovered-signature store: a sample, not a search
        sample = [node] + [self.mninfo[i].node for i in (0, 32, 64)]
        for n in sample:
            for height in (LEAD_START + 4, 4950, LAST_POW_BLOCK, H - 1):
                assert not n.quorum("hasrecsig", Q60_TYPE, self.clsig_request_id(height),
                                    node.getblockhash(height)), (n.index, height)
        assert not self.q60_signed(node, tx1), "a Q60 InstantSend signature exists below H"
        self.mark("no lock for the sampled heights on the sampled nodes, and no Q60 signature below H")

        # ---- proof 3: block H, ChainLocked by Q60 --------------------------
        h_hash = self.stake_exactly_to(node, H)
        assert_equal(node.getblockcount(), H)
        self.assert_pos_regime(node, H)
        self.wait_for_chainlock(node, h_hash, timeout=CL_AT_H_TIMEOUT)
        lock_at_h = node.getbestchainlock()
        assert_equal((lock_at_h["height"], lock_at_h["blockhash"], lock_at_h["llmqType"]), (H, h_hash, Q60_NAME))
        assert node.verifychainlock(h_hash, lock_at_h["signature"], H)
        [signer_at_h] = self.assert_lock_profile(node, lock_at_h, expect_q60=True)
        assert signer_at_h in self.lead_quorums, signer_at_h
        self.assert_lock_profile(node, legacy_lock, expect_q60=False)   # still verifies, forever
        self.all_answer("the lock at H everywhere",
                        lambda n: n.getbestchainlock()["blockhash"] == h_hash, CL_AT_H_TIMEOUT)
        self.mark("block H = %d is ChainLocked by %s on every node" % (H, Q60_NAME))

        # ---- proof 4: a real Q60 InstantSend lock --------------------------
        self.log.info("A fee-paying transaction at H, locked by the Q60 quorum")
        t_sent = self.mocktime
        tx2 = self.send_fee_tx(node, Decimal("0.0025"))
        self.sync_mempools(timeout=SYNC_TIMEOUT)
        self.wait_for_internal_islock(node, tx2)
        for i in (0, 32, 64):
            self.wait_for_internal_islock(self.mninfo[i].node, tx2)
        assert self.q60_signed(node, tx2, timeout=30), "the lock was not signed by the Q60 quorum"
        islocks = node.getislocks([tx2])
        assert_equal(len(islocks), 1)
        assert_equal(node.getblock(islocks[0]["cycleHash"])["height"] % Q60_DKG_INTERVAL, 0)

        h17_hash = self.stake_exactly_to(node, H + 1)
        assert tx2 in node.getblock(h17_hash)["tx"], "the locked transaction was not mined in block H + 1"
        assert self.mocktime - t_sent < 120, \
            "the transaction was mined after the unlocked wait, not because it was locked"
        supply_after = self.supply(node)
        self.mark("the locked transaction was mined in %d mock seconds, well inside the unlocked wait"
                  % (self.mocktime - t_sent))

        # ---- the modifier switch, as soon as block H exists ----------------
        before, after = self.check_modifiers(node, LAST_POW_BLOCK + 1, H + 1)
        assert_equal((before, after), (H - LAST_POW_BLOCK - 2, 2))
        self.log.info("Every stake modifier matches the rule of its own side: %d below H, %d at or above", before, after)

        # ---- proof 5, on three staked blocks -------------------------------
        for height in (H - 1, H, H + 1):
            self.assert_mn_payment_in_pos_coinbase(node, height)
        self.mark("the masternode payment equals the computed amount and split in staked blocks either side of H")

        # ---- proof 6: Q60 quorums form on staked blocks ---------------------
        self.stake_exactly_to(node, STAKED_FIRST_BASE - 1)
        self.set_dkg(True)
        self.log.info("DKG on: %d Q60 cycles on staked blocks, bases %d..%d, every phase waited for",
                      len(STAKED_BASES), STAKED_FIRST_BASE, STAKED_LAST_BASE)
        self.staked_quorums = []
        self.epoch_quorum = None
        self.cycle_record = []
        is_checked = False
        for i, base_height in enumerate(STAKED_BASES):
            quorum, mined_at = self.stake_q60(node, base_height)
            self.staked_quorums.append(quorum)
            self.cycle_record.append((base_height, mined_at))
            self.mark("staked Q60 quorum %d of %d, base %d, its commitment mined in staked block %d"
                      % (i + 1, len(STAKED_BASES), base_height, mined_at))
            if base_height in CL_CHECK_AFTER:
                # A quorum signs only from eight blocks after its commitment (SIGN_HEIGHT_OFFSET), as
                # mine_quorum's own trailing eight blocks allow for; still well below the next base.
                self.stake_exactly_to(node, mined_at + 8, exact=False)
                assert node.getblockcount() < base_height + Q60_DKG_INTERVAL
                # The active set is the four newest quorums; after the fourth staked one it holds no lead quorum.
                active = node.quorum("list")[Q60_NAME]
                assert set(active) <= set(self.staked_quorums), ("a lead quorum is still active", active)
                lock = self.assert_tip_locked_by_staked_quorum(node, "after base %d" % base_height)
                self.mark("the tip %d is ChainLocked by a quorum formed on staked blocks" % lock["height"])
                if not is_checked:
                    self.log.info("An InstantSend lock signed by a quorum formed on staked blocks")
                    tx3 = self.send_fee_tx(node, Decimal("0.0035"))
                    self.sync_mempools(timeout=SYNC_TIMEOUT)
                    self.wait_for_internal_islock(node, tx3)
                    [islock] = node.getislocks([tx3])
                    assert "signature" in islock, islock
                    signers = self.signers_of(node, Q60_TYPE, Q60_NAME, self.islock_request_id(node, tx3),
                                              tx3, islock["signature"])
                    assert_equal(len(signers), 1)
                    assert signers[0] in self.staked_quorums, signers
                    is_checked = True
        assert is_checked
        assert_equal(len(set(self.staked_quorums)), len(STAKED_BASES))
        self.mark("%d Q60 quorums formed on staked blocks; the lead quorums left the active set" % len(STAKED_BASES))

        # ---- proof 7: the Sentinel layer, from H + 576 ----------------------
        self.stake_exactly_to(node, DSL_START - 1)
        self.log.info("AT %d, one block below its start, the Sentinel layer is dormant on every node", DSL_START - 1)
        self.assert_sentinel_dormant()
        self.assert_tip_locked_by_staked_quorum(node, "below the Sentinel start")

        signed_at = self.walk_first_epoch(node)
        # No further cycle: the next base is the commitment block itself.
        self.set_dkg(False)
        self.stake_exactly_to(node, FIRST_COMMITMENT)
        assert_equal(node.getblockcount(), FIRST_COMMITMENT)
        commitment, msg_hash, signature = self.commitment_at(node, FIRST_COMMITMENT)
        base = node.getblockhash(DSL_START)
        assert_equal((commitment["version"], commitment["epoch"]), (2, FIRST_EPOCH))
        assert_equal(commitment["epochBlockHash"], base)
        assert_equal(commitment["llmqType"], Q60_TYPE)
        assert_equal(commitment["size"], MN_COUNT)
        assert_equal((commitment["missedCount"], commitment["missedIndices"], commitment["unobservedIndices"]),
                     (0, [], []))
        assert_equal(commitment["observedCount"], commitment["size"])
        # The epoch selects its quorum eight blocks below its base: one formed on
        # staked blocks, not the one that formed beside the epoch.
        assert commitment["quorumHash"] in self.staked_quorums, commitment["quorumHash"]
        assert commitment["quorumHash"] != self.epoch_quorum
        request_id = self.dsl_request_id(node, FIRST_EPOCH)
        assert node.quorum("hasrecsig", Q60_TYPE, request_id, msg_hash)
        assert_equal(self.signers_of(node, Q60_TYPE, Q60_NAME, request_id, msg_hash, signature),
                     [commitment["quorumHash"]])
        assert_equal(self.signers_of(node, LEGACY_TYPE, LEGACY_NAME, request_id, msg_hash, signature), [])
        assert not node.quorum("verify", Q60_TYPE, request_id, flip_last_bit(msg_hash),
                               signature, commitment["quorumHash"])

        block = node.getblock(node.getblockhash(FIRST_COMMITMENT), 2)
        assert_equal(block["flags"], "proof-of-stake")
        assert_equal(block["nonce"], 0)
        assert_equal(block["tx"][0]["type"], 5)     # the coinbase carries the CbTx payload
        assert_equal(block["tx"][2]["type"], DSL_TX_TYPE)
        # the commitment is attached at vtx[1] and the coinstake is inserted
        # before it, so the block rule has to find it at an index the
        # proof-of-work path never puts it at
        self.assert_mn_payment_in_pos_coinbase(node, FIRST_COMMITMENT)
        self.mark("the first Sentinel commitment is in a STAKED block, at index 2, signed by a staked-era quorum")

        # ---- the sweep: proofs 1 and 8 over the whole staked chain ------------
        self.stake_exactly_to(node, FINAL_HEIGHT)
        final_lock = self.assert_tip_locked_by_staked_quorum(node, "the final block")

        before, after = self.check_modifiers(node, LAST_POW_BLOCK + 1, FINAL_HEIGHT)
        assert_equal(before, H - LAST_POW_BLOCK - 2)
        assert_equal(after, FINAL_HEIGHT - H + 1)

        payees = set()
        prev_time = None
        for height in range(LAST_POW_BLOCK + 1, FINAL_HEIGHT + 1):
            block = self.assert_pos_regime(node, height, prev_time)
            prev_time = block["time"]
            for out in block["tx"][0]["vout"][1:]:
                address = out["scriptPubKey"].get("address")
                assert address in self.payee_by_address, (height, address)
                payees.add(self.payee_by_address[address])
        assert len(payees) >= 30, "only %d distinct masternodes were paid in %d staked blocks" % (
            len(payees), FINAL_HEIGHT - LAST_POW_BLOCK)

        listed = node.protx("list", "registered", True)
        assert_equal(len(listed), MN_COUNT)
        for mn in listed:
            assert_equal((mn["state"]["PoSePenalty"], mn["state"]["PoSeBanHeight"]), (0, -1))
        self.sync_blocks(self.nodes, timeout=SYNC_TIMEOUT)
        self.mark("every staked block is well-formed, %d masternodes were paid, and nobody carries a penalty"
                  % len(payees))

        # ---- proof 9: re-validation ------------------------------------------
        self.log.info("The staking node re-validates the whole chain from its own block files")
        quorums_before = node.quorum("list", 100)
        tip_before = node.getbestblockhash()
        # Block times run a little ahead of the mock clock; a restarted node
        # must find every one of them in its past.
        self.bump_mocktime(60)
        set_node_times(self.nodes, self.mocktime)
        asked = time.time()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: node.getblockcount() == FINAL_HEIGHT and node.getbestblockhash() == tip_before,
                        timeout=REINDEX_TIMEOUT, sleep=1)
        reindex_wait = time.time() - asked
        self.log.info("  same tip after %.1f s", reindex_wait)
        assert_equal(node.quorum("list", 100), quorums_before)
        for lock in (legacy_lock, lock_at_h, final_lock):
            assert node.verifychainlock(lock["blockhash"], lock["signature"], lock["height"])
        again = self.commitment_at(node, FIRST_COMMITMENT)
        assert_equal(again, (commitment, msg_hash, signature))

        self.log.info("  and it drives the network again: its next block is accepted everywhere and ChainLocked")
        force_finish_mnsync(node)
        # FROM the staking node, not to it: back up, it opens quorum connections
        # to masternodes by itself, and a masternode that then dials it as well
        # is refused, so connect_nodes would wait for an inbound peer that never comes.
        for mn in self.mninfo:
            self.connect_nodes(0, mn.nodeIdx)
        self.stake_exactly_to(node, FINAL_HEIGHT + 1)
        self.assert_tip_locked_by_staked_quorum(node, "after the restart")

        self.log.info("RECORD H=%d dsl=%d commitment=%d staked=%d staked_q60=%d cycles=%s modifiers=%d/%d "
                      "payees=%d signed_at=+%d supply_step=%s overruns=%s reindex=%.1fs wall=%ds",
                      H, DSL_START, FIRST_COMMITMENT, FINAL_HEIGHT + 1 - LAST_POW_BLOCK, len(self.staked_quorums),
                      ",".join("%d@%d" % c for c in self.cycle_record), before, after, len(payees), signed_at,
                      supply_after - supply_before, self.overruns, reindex_wait, time.time() - self.started)


if __name__ == "__main__":
    V23JointActivationPosMnTest().main()
