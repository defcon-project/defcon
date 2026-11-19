#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""How a masternode answers a mixing request for a DFCN-scale denomination (CJ4-A).

Bits 5-7 of the wire table (100.001, 1000.01 and 10 000.1 DFCN) open for new
sessions only from the v23.1 height H, counted by the next block, and never
while H is unset. Until then a masternode answers them as v23.0.0 answers a bit
it does not know: ERR_DENOM, no penalty, before the collateral is looked at. In
an open session a different DFCN-scale bit is refused the same way, while a
different old bit keeps today's order (the collateral first). A session opened
for a DFCN-scale bit at or above H keeps admitting its own bit through the usual
collateral check after the tip falls below H.

Every answer is read from the status update (dssu) the masternode sends the peer.
"""

from decimal import Decimal

from test_framework.messages import (
    CCoinJoinAccept,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    from_hex,
    msg_dsa,
)
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
    OP_DUP,
    OP_EQUALVERIFY,
    OP_HASH160,
)
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

OLD_BIT = 1          # 10.0001 DFCN
OTHER_OLD_BIT = 2    # 1.00001 DFCN
NEW_BIT = 32         # 100.001 DFCN
H = 400
NEVER = 2**31 - 1    # the largest height stands for never, as an unset gate holds it
P2PKH = CScript([OP_DUP, OP_HASH160, b"\x01" * 20, OP_EQUALVERIFY, OP_CHECKSIG])

# src/coinjoin/coinjoin.h: PoolStatusUpdate and PoolMessage.
STATUS_REJECTED = 0
STATUS_ACCEPTED = 1
ERR_DENOM = 1
ERR_INVALID_COLLATERAL = 5
MSG_NOERR = 19


class StatusPeer(P2PInterface):
    """Keeps every status update (dssu) the masternode sends."""

    def __init__(self):
        super().__init__()
        self.statuses = []

    def on_dssu(self, message):
        self.statuses.append(message.dssu)


class P2PCoinJoinDfcnDenomsTest(DashTestFramework):
    def set_test_params(self):
        gates = ["-testactivationheight=superblocksretired@%d" % H,
                 "-testactivationheight=mn_rr@%d" % NEVER]
        with_h = gates + ["-testactivationheight=v231@%d" % H]
        # node1, the first masternode, leaves H unset; every other node sets it to 400.
        self.set_dash_test_params(4, 3, [with_h,
                                         gates + ["-debug=coinjoin"],
                                         with_h + ["-debug=coinjoin"],
                                         with_h + ["-debug=coinjoin"]])

    def reserve_collateral(self):
        """A signed collateral the masternodes accept: a coin of node0, 0.0001 DFCN to fees. The coin is mature
        now, below the lowest tip any masternode reaches here, and locked against other use."""
        node = self.nodes[0]
        utxo = next(u for u in node.listunspent(1) if u["amount"] >= 1)
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        change = utxo["amount"] - Decimal("0.0001")
        raw = node.createrawtransaction([{"txid": utxo["txid"], "vout": utxo["vout"]}], {node.getnewaddress(): change})
        return from_hex(CTransaction(), node.signrawtransactionwithwallet(raw)["hex"])

    def invalid_collateral(self):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(0xdead, 0xffffffff))]
        tx.vout = [CTxOut(nValue=COIN // 1000, scriptPubKey=P2PKH)]
        return tx

    def ask(self, peer, denom, collateral):
        """Sends a mixing request and returns the masternode's one status update for it."""
        with p2p_lock:
            seen = len(peer.statuses)
        peer.send_and_ping(msg_dsa(CCoinJoinAccept(denom, collateral)))
        peer.wait_until(lambda: len(peer.statuses) > seen)
        with p2p_lock:
            assert_equal(len(peer.statuses), seen + 1)
            return peer.statuses[-1]

    def expect(self, what, status, update, message_id, session):
        """Checks (nStatusUpdate, nMessageID, nSessionID) of an answer; a mismatch names the step."""
        got = (status.nStatusUpdate, status.nMessageID, status.nSessionID)
        if got != (update, message_id, session):
            raise AssertionError("%s: got %s, expected %s" % (what, got, (update, message_id, session)))

    def run_test(self):
        node = self.nodes[0]
        # The reorg below takes one masternode down to H-3; every positive answer spends a coin mature there.
        assert node.getblockcount() < H - 3, "the setup ran past H"
        collaterals = [self.reserve_collateral() for _ in range(3)]

        self.log.info("H unset: a new session for a DFCN-scale bit is refused with ERR_DENOM, before the collateral")
        unset = self.mninfo[0].node
        peer = unset.add_p2p_connection(StatusPeer())
        self.expect("H unset, a new DFCN-scale session", self.ask(peer, NEW_BIT, self.invalid_collateral()),
                    STATUS_REJECTED, ERR_DENOM, 0)

        self.log.info("H unset: an old-bit session opens")
        opener = unset.add_p2p_connection(StatusPeer())
        opened = self.ask(opener, OLD_BIT, collaterals.pop())
        assert opened.nSessionID > 0
        self.expect("H unset, an old-bit session", opened, STATUS_ACCEPTED, MSG_NOERR, opened.nSessionID)

        self.log.info("In that session a DFCN-scale bit is refused with ERR_DENOM, before the collateral")
        late = unset.add_p2p_connection(StatusPeer())
        self.expect("a DFCN-scale bit in the old-bit session", self.ask(late, NEW_BIT, self.invalid_collateral()),
                    STATUS_REJECTED, ERR_DENOM, opened.nSessionID)

        self.log.info("A different old bit still has its collateral checked first, as today")
        self.expect("a different old bit in the old-bit session", self.ask(late, OTHER_OLD_BIT, self.invalid_collateral()),
                    STATUS_REJECTED, ERR_INVALID_COLLATERAL, opened.nSessionID)

        self.log.info("H=400, tip H-2 (next block H-1): a new DFCN-scale session is still refused")
        self.generate(node, H - 2 - node.getblockcount())
        mn = self.mninfo[1].node
        assert_equal(mn.getblockcount(), H - 2)
        opener = mn.add_p2p_connection(StatusPeer())
        self.expect("tip H-2, a new DFCN-scale session", self.ask(opener, NEW_BIT, self.invalid_collateral()),
                    STATUS_REJECTED, ERR_DENOM, 0)

        self.log.info("Tip H-1 (next block H): the same masternode opens it")
        self.generate(node, 1)
        assert_equal(mn.getblockcount(), H - 1)
        with mn.assert_debug_log(["nSessionDenom: %d" % NEW_BIT]):
            opened = self.ask(opener, NEW_BIT, collaterals.pop())
        assert opened.nSessionID > 0
        self.expect("tip H-1, a new DFCN-scale session", opened, STATUS_ACCEPTED, MSG_NOERR, opened.nSessionID)

        self.log.info("After the tip falls below H the session's own bit still goes through the collateral check")
        mn.invalidateblock(mn.getblockhash(H - 2))
        assert_equal(mn.getblockcount(), H - 3)
        joiner = mn.add_p2p_connection(StatusPeer())
        self.expect("after the reorg, the session's bit with an invalid collateral",
                    self.ask(joiner, NEW_BIT, self.invalid_collateral()),
                    STATUS_REJECTED, ERR_INVALID_COLLATERAL, opened.nSessionID)
        self.expect("after the reorg, the session's bit with a valid collateral", self.ask(joiner, NEW_BIT, collaterals.pop()),
                    STATUS_ACCEPTED, MSG_NOERR, opened.nSessionID)


if __name__ == '__main__':
    P2PCoinJoinDfcnDenomsTest().main()
