#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The mixing client's DFCN-scale attempt on a regtest network with masternodes (CJ4-B2b4).

The client wallet holds a 100.001 DFCN candidate and 0.0013 DFCN of ordinary funds, from one payment of another
wallet, so its old path always stops at "Not enough funds to mix." (no collateral yet). H is 400, U (cj4use) 410:
 - on the tip U-2 the client's gate is closed (the next block is U-1): the attempt is the old one and stops;
 - on the tip U-1 it is open: the attempt is the DFCN-scale one, and its maker makes a collateral from the 0.0013;
 - once that collateral is mined, the next attempt starts a queue of bit 5 at a masternode (the session's
   denomination 100.001), and the masternode opens a session for bit 5.
"""

from decimal import Decimal
import re

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

H = 400
U = 410
NEVER = 2**31 - 1
NEW_BIT = 32                   # 100.001 DFCN
CANDIDATE = Decimal("100.001")
ORDINARY = Decimal("0.0013")   # less than the old path's minimum without collateral: it stops at "Not enough funds"


class P2PCoinJoinDfcnAttemptTest(DashTestFramework):
    def set_test_params(self):
        heights = ["-testactivationheight=superblocksretired@%d" % H, "-testactivationheight=mn_rr@%d" % NEVER,
                   "-testactivationheight=v231@%d" % H, "-testactivationheight=cj4use@%d" % U]
        client = heights + ["-debug=coinjoin", "-coinjoinsessions=1", "-coinjoinamount=2", "-createwalletbackups=10"]
        # node0 the controller, node1 the client, node2-node4 the masternodes.
        self.set_dash_test_params(5, 3, [heights, client] + [heights + ["-debug=coinjoin"]] * 3)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def logged_text(self, node, offset):
        with open(node.debug_log_path, "rb") as log:
            log.seek(offset)
            return log.read()

    def wait_logged(self, node, offset, pattern):
        """Waits for a line of the node's log after the offset that matches the pattern; returns the match."""
        self.wait_until(lambda: re.search(pattern, self.logged_text(node, offset)) is not None, timeout=30)
        return re.search(pattern, self.logged_text(node, offset))

    def attempt(self, client, wallet, expected_log):
        """One automatic attempt through 'coinjoin start' (it runs one at once), then mixing stopped again."""
        with client.assert_debug_log([expected_log], timeout=10):
            response = wallet.coinjoin("start")
        wallet.coinjoin("stop")
        return response

    def run_test(self):
        controller = self.nodes[0]
        client = self.nodes[1]
        assert controller.getblockcount() < U - 3, "the setup ran past U"
        # No quorum signs InstantSend locks here, and the miner would hold an unlocked transaction back for ten minutes:
        # InstantSend off, so that a payment is mined in the next block.
        controller.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        # A descriptor wallet mixes: a legacy wallet created after startup has no automatic backup, and its client
        # stops mixing for that (CheckAutomaticBackup).
        client.createwallet(wallet_name="mixer", descriptors=True)
        mixer = client.get_wallet_rpc("mixer")

        self.log.info("One payment of another wallet: a candidate and 0.0013 DFCN, both to new client addresses")
        candidate_address = mixer.getnewaddress()
        ordinary_address = mixer.getnewaddress()
        controller.sendmany("", {candidate_address: CANDIDATE, ordinary_address: ORDINARY})
        self.generate(controller, 1)
        coins = {Decimal(str(u["amount"])): u for u in mixer.listunspent(1)}
        assert_equal(sorted(coins), [ORDINARY, CANDIDATE])  # exact: no fee taken from either
        assert all(len(c["scriptPubKey"]) == 50 and c["scriptPubKey"].startswith("76a914") and
                   c["scriptPubKey"].endswith("88ac") for c in coins.values())  # P2PKH
        candidate = coins[CANDIDATE]
        ordinary = coins[ORDINARY]

        self.log.info("Tip U-2: the gate is closed, the attempt is the old one, which stops")
        self.generate(controller, U - 2 - controller.getblockcount())
        assert_equal(client.getblockcount(), U - 2)
        self.attempt(client, mixer, "DoAutomaticDenominating -- Not enough funds to mix.")
        assert any(u["txid"] == ordinary["txid"] and u["vout"] == ordinary["vout"] for u in mixer.listunspent(0))

        self.log.info("Tip U-1: the gate is open, the DFCN-scale attempt makes a collateral from the ordinary funds")
        self.generate(controller, 1)
        assert_equal(client.getblockcount(), U - 1)
        response = self.attempt(client, mixer, "MakeCollateralAmounts -- Start")
        assert_equal(response, "Mixing started successfully")
        unspent = mixer.listunspent(0)
        assert not any(u["txid"] == ordinary["txid"] and u["vout"] == ordinary["vout"] for u in unspent)
        assert any(u["txid"] == candidate["txid"] and u["vout"] == candidate["vout"] for u in unspent)

        self.log.info("With the collateral mined, the next attempt starts a bit-5 queue at a masternode, which opens it")
        self.sync_mempools([client, controller])
        self.generate(controller, 1)
        assert all(u["confirmations"] > 0 for u in mixer.listunspent(0))
        # The masternode the client names opens a session for bit 32, and the client holds that session's id.
        client_offset = client.debug_log_bytes()
        offsets = {mn.proTxHash: mn.node.debug_log_bytes() for mn in self.mninfo}
        assert_equal(mixer.coinjoin("start"), "Mixing started successfully")
        self.wait_logged(client, client_offset, rb"StartNewDfcnScaleQueue -- pending connection, nSessionDenom: %d " % NEW_BIT)
        session = mixer.getcoinjoininfo()["sessions"][0]
        assert_equal(Decimal(str(session["denomination"])), CANDIDATE)
        mn = next(mn for mn in self.mninfo if mn.proTxHash == session["protxhash"])
        opened = self.wait_logged(mn.node, offsets[mn.proTxHash],
                                  rb"CreateNewSession -- new session created, nSessionID: (\d+)  nSessionDenom: (\d+) ")
        assert_equal(int(opened.group(2)), NEW_BIT)
        accepted = self.wait_logged(client, client_offset, rb"Set nSessionID to (\d+)\.")
        assert_equal(accepted.group(1), opened.group(1))
        mixer.coinjoin("stop")


if __name__ == '__main__':
    P2PCoinJoinDfcnAttemptTest().main()
