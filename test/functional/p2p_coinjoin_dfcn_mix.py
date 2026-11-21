#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A real mix of a DFCN-scale amount between two wallets on a regtest network with masternodes (CJ4-B2b4).

H is 400, U (cj4use) 410. Each of two client wallets holds a 100.001 DFCN candidate and 0.0013 DFCN of ordinary
funds, so its old path stops at "Not enough funds to mix.". From U each makes its collateral from the 0.0013; then
the first starts a bit-5 queue at the masternode it names, which opens a session for bit 32; the second joins that
queue at the same masternode, which takes it into the same session; with mocktime past the queue time the masternode
starts the session, and both mix: one transaction, the two candidates in and two outputs of 100.001 DFCN out, mined,
and each wallet's new coin counts one round where the candidate counted none.
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
QUEUE_TIMEOUT = 30             # src/coinjoin/coinjoin.h: COINJOIN_QUEUE_TIMEOUT (a client waits 10 s longer)


def is_p2pkh(script_hex):
    return len(script_hex) == 50 and script_hex.startswith("76a914") and script_hex.endswith("88ac")


class P2PCoinJoinDfcnMixTest(DashTestFramework):
    def set_test_params(self):
        heights = ["-testactivationheight=superblocksretired@%d" % H, "-testactivationheight=mn_rr@%d" % NEVER,
                   "-testactivationheight=v231@%d" % H, "-testactivationheight=cj4use@%d" % U]
        client = heights + ["-debug=coinjoin", "-coinjoinsessions=1", "-coinjoinamount=2"]
        # node0 the controller, node1 and node2 the clients, node3-node5 the masternodes.
        self.set_dash_test_params(6, 3, [heights, client, client] + [heights + ["-debug=coinjoin"]] * 3)

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

    def mix_in_mempool(self):
        for txid in self.nodes[0].getrawmempool():
            tx = self.nodes[0].getrawtransaction(txid, True)
            if len(tx["vout"]) == 2 and all(Decimal(str(out["value"])) == CANDIDATE for out in tx["vout"]):
                return tx
        return None

    def run_test(self):
        controller = self.nodes[0]
        clients = [self.nodes[1], self.nodes[2]]
        assert controller.getblockcount() < U - 3, "the setup ran past U"
        # No quorum signs InstantSend locks here, and the miner would hold an unlocked transaction back: off.
        controller.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("Each client: a descriptor wallet with a candidate and 0.0013 DFCN, paid by another wallet")
        wallets = []
        outputs = {}
        for client in clients:
            # A legacy wallet created after startup has no automatic backup, and its client stops mixing for that.
            client.createwallet(wallet_name="mixer", descriptors=True)
            wallet = client.get_wallet_rpc("mixer")
            wallets.append(wallet)
            outputs[wallet.getnewaddress()] = CANDIDATE
            outputs[wallet.getnewaddress()] = ORDINARY
        controller.sendmany("", outputs)
        self.generate(controller, U - 1 - controller.getblockcount())
        assert_equal(controller.getblockcount(), U - 1)
        candidates = []
        for wallet in wallets:
            coins = {Decimal(str(u["amount"])): u for u in wallet.listunspent(1)}
            assert_equal(sorted(coins), [ORDINARY, CANDIDATE])  # exact: no fee taken from either
            assert all(is_p2pkh(c["scriptPubKey"]) for c in coins.values())
            assert_equal(coins[CANDIDATE]["coinjoin_rounds"], 0)  # an ordinary incoming coin: no round yet
            candidates.append(coins[CANDIDATE])

        self.log.info("Tip U-1: each client's DFCN-scale attempt makes its collateral from the 0.0013 DFCN")
        for client, wallet in zip(clients, wallets):
            with client.assert_debug_log(["MakeCollateralAmounts -- Start"], timeout=10):
                assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
            wallet.coinjoin("stop")
        self.sync_mempools()
        self.generate(controller, 1)
        for wallet in wallets:
            assert all(u["confirmations"] > 0 for u in wallet.listunspent(0))

        self.log.info("The first client starts a bit-5 queue; the masternode it names opens a session for bit 32")
        client_offsets = [client.debug_log_bytes() for client in clients]
        mn_offsets = {mn.proTxHash: mn.node.debug_log_bytes() for mn in self.mninfo}
        assert_equal(wallets[0].coinjoin("start"), "Mixing started successfully")
        self.wait_logged(clients[0], client_offsets[0],
                         rb"StartNewDfcnScaleQueue -- pending connection, nSessionDenom: %d " % NEW_BIT)
        protx = wallets[0].getcoinjoininfo()["sessions"][0]["protxhash"]
        mn = next(mn for mn in self.mninfo if mn.proTxHash == protx)
        opened = self.wait_logged(mn.node, mn_offsets[protx],
                                  rb"CreateNewSession -- new session created, nSessionID: (\d+)  nSessionDenom: (\d+) ")
        assert_equal(int(opened.group(2)), NEW_BIT)
        session_id = opened.group(1)
        assert_equal(self.wait_logged(clients[0], client_offsets[0], rb"Set nSessionID to (\d+)\.").group(1), session_id)

        self.log.info("The second client sees the queue and joins it at the same masternode, in the same session")
        self.wait_until(lambda: wallets[1].getcoinjoininfo()["queue_size"] > 0, timeout=30)
        assert_equal(wallets[1].coinjoin("start"), "Mixing started successfully")
        self.wait_logged(clients[1], client_offsets[1],
                         rb"JoinExistingDfcnScaleQueue -- pending connection \(from queue\): nSessionDenom: %d " % NEW_BIT)
        assert_equal(wallets[1].getcoinjoininfo()["sessions"][0]["protxhash"], protx)
        joined = self.wait_logged(mn.node, mn_offsets[protx], rb"AddUserToExistingSession -- new user accepted, nSessionID: (\d+) ")
        assert_equal(joined.group(1), session_id)
        assert_equal(self.wait_logged(clients[1], client_offsets[1], rb"Set nSessionID to (\d+)\.").group(1), session_id)

        self.log.info("With mocktime past the queue time the masternode starts the session, and the two wallets mix")
        ready_offset = mn.node.debug_log_bytes()
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % NEW_BIT)
        self.wait_until(lambda: self.mix_in_mempool() is not None, timeout=60)
        mix = self.mix_in_mempool()
        assert_equal({(vin["txid"], vin["vout"]) for vin in mix["vin"]}, {(c["txid"], c["vout"]) for c in candidates})
        for wallet in wallets:
            wallet.coinjoin("stop")
        self.generate(controller, 1)

        self.log.info("Each wallet's new coin counts one round")
        for wallet in wallets:
            new = [u for u in wallet.listunspent(1) if u["txid"] == mix["txid"]]
            assert_equal(len(new), 1)
            assert_equal(Decimal(str(new[0]["amount"])), CANDIDATE)
            assert_equal(new[0]["coinjoin_rounds"], 1)


if __name__ == '__main__':
    P2PCoinJoinDfcnMixTest().main()
