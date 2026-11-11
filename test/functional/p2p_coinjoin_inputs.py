#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test how a masternode checks the collateral of a mixing request from a peer.

The collateral of a DSACCEPT may spend an output of an InstantSend-locked
transaction that is still in the mempool. The masternode looks the input up and
rejects the request when the output does not exist or the collateral does not
pay its fee, and keeps serving either way. The amounts of a collateral come from
the peer as well: an output amount out of range, outputs or inputs that add up
past the money range (the same coin named many times) are refused before they
are added up.
"""

from test_framework.messages import (
    CCoinJoinAccept,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    MAX_MONEY,
    msg_dsa,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
    OP_DUP,
    OP_EQUALVERIFY,
    OP_HASH160,
)
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

# The largest standard denomination bit, a valid nDenom (CoinJoin::IsValidDenomination).
DENOM = 1
P2PKH = CScript([OP_DUP, OP_HASH160, b"\x01" * 20, OP_EQUALVERIFY, OP_CHECKSIG])


class P2PCoinJoinInputsTest(DashTestFramework):
    def set_test_params(self):
        # -whitelist avoids the trickling logic on node0; the masternodes log their CoinJoin decisions.
        self.set_dash_test_params(6, 5, [["-whitelist=127.0.0.1"]] + [["-debug=coinjoin"]] * 5)

    def collateral(self, txid, n, *values):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        tx.vout = [CTxOut(nValue=value, scriptPubKey=P2PKH) for value in values]
        tx.nLockTime = 0
        return tx

    def run_test(self):
        node = self.nodes[0]
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.mine_instantsend_quorum()
        self.bump_mocktime(1)
        self.generate(node, 8)

        mn = self.mninfo[0].node
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        self.wait_for_instantlock(txid, node)
        self.wait_for_instantlock(txid, mn)
        assert txid in mn.getrawmempool()
        vouts = mn.getrawtransaction(txid, True)["vout"]
        n_outputs = len(vouts)
        # The 1-coin payment, not the change, so that paying out 2 coins is always short of the input.
        payment = next(out["n"] for out in vouts if out["value"] == 1)

        peer = mn.add_p2p_connection(P2PInterface())

        self.log.info("Collateral spending an output index far past the locked mempool transaction's outputs")
        with mn.assert_debug_log(["missing, spent or non-locked mempool input", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, self.collateral(txid, 0xffffffff, COIN // 1000))))

        self.log.info("Collateral spending the first index past the outputs")
        with mn.assert_debug_log(["missing, spent or non-locked mempool input", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, self.collateral(txid, n_outputs, COIN // 1000))))

        self.log.info("Collateral spending a real output of the locked transaction but paying out more than it has")
        with mn.assert_debug_log(["did not include enough fees", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, self.collateral(txid, payment, 2 * COIN))))

        self.log.info("Collateral whose output amounts add up past the largest 64-bit amount")
        with mn.assert_debug_log(["output amount out of range", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, self.collateral(txid, payment, 2**63 - 1, 1))))

        self.log.info("Collateral whose output amounts are each in range but add up past the money range")
        with mn.assert_debug_log(["output amount out of range", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, self.collateral(txid, payment, MAX_MONEY, 1))))

        self.log.info("Collateral paying out a negative amount")
        with mn.assert_debug_log(["output amount out of range", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, self.collateral(txid, payment, -1))))

        self.log.info("Collateral naming the same confirmed coin until its inputs add up past the largest 64-bit amount")
        coin = max(node.listunspent(), key=lambda utxo: utxo["amount"])
        value = int(coin["amount"] * COIN)
        repeats = (2**63 - 1) // value + 1
        assert repeats <= 20000, "the largest coin, %s, is too small for a collateral of a sane size" % coin["amount"]
        assert repeats * value > MAX_MONEY
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(coin["txid"], 16), coin["vout"]))] * repeats
        tx.vout = [CTxOut(nValue=COIN // 1000, scriptPubKey=P2PKH)]
        tx.nLockTime = 0
        with mn.assert_debug_log(["input amount out of range", "collateral not valid"]):
            peer.send_and_ping(msg_dsa(CCoinJoinAccept(DENOM, tx)))

        self.log.info("The masternode keeps serving")
        assert_equal(mn.getbestblockhash(), node.getbestblockhash())
        assert txid in mn.getrawmempool()
        self.generate(node, 1)
        assert txid not in mn.getrawmempool()


if __name__ == '__main__':
    P2PCoinJoinInputsTest().main()
