#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""An old-amount mix between H and U while each wallet holds DFCN-scale candidates (CJ4-B2d2a).

H is 400, U (cj4use) 430. Each of two client wallets holds two coins of exactly 1.00001 DFCN (an old denomination),
0.0013 DFCN of ordinary funds and two 1000.01 DFCN candidates, one of them locked by the user before mixing starts,
all paid by another wallet, and an old target of 10 DFCN. Between H and U the old path runs: each makes its
collateral from the 0.0013 and no denominations (a candidate is no funds to it, whatever its target); the first
starts a queue for 1.00001 at the masternode it names, the second joins it there, and with mocktime past the queue
time both mix: one transaction whose inputs are the wallets' 1.00001 coins, at least one of each, and whose outputs
are all 1.00001, mined, each new coin one round. Neither candidate is touched: both stay unspent, the user's lock
stays, and no DFCN-scale attempt runs. Two peers of the mixing masternode, one at 70243 and one at 70242 (what
v23.0.0 advertises), are both offered the old DSTX and get it.
"""

from decimal import Decimal
import re
import time

from test_framework.messages import MSG_DSTX
from test_framework.p2p import P2PVersionedPeer, p2p_lock
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

H = 400
U = 430
NEVER = 2**31 - 1
OLD_BIT = 2                    # 1.00001 DFCN (src/coinjoin/common.h: vecDenominationsByBit)
OLD_DENOM = Decimal("1.00001")
CANDIDATE = Decimal("1000.01")
ORDINARY = Decimal("0.0013")   # less than the old path's minimum without collateral: no new denominations from it
QUEUE_TIMEOUT = 30             # src/coinjoin/coinjoin.h: COINJOIN_QUEUE_TIMEOUT (a client waits 10 s longer)
OLD_VERSION = 70242            # src/version.h: Q60_SWITCHOVER_PROTO_VERSION, what v23.0.0 advertises
NEW_VERSION = 70243            # src/version.h: COINJOIN_DFCN_DENOMS_PROTO_VERSION


def outpoint(coin):
    return (coin["txid"], coin["vout"])


class P2PCoinJoinDfcnOldMixTest(DashTestFramework):
    def set_test_params(self):
        heights = ["-testactivationheight=superblocksretired@%d" % H, "-testactivationheight=mn_rr@%d" % NEVER,
                   "-testactivationheight=v231@%d" % H, "-testactivationheight=cj4use@%d" % U]
        # The old target, 10 DFCN, is more than the wallets' two 1.00001 coins: the old path would make denominations
        # from any funds it may use, and a 1000.01 candidate is none.
        client = heights + ["-debug=coinjoin", "-coinjoinsessions=1", "-coinjoinamount=10"]
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
            if len(tx["vin"]) == len(tx["vout"]) >= 2 and all(Decimal(str(out["value"])) == OLD_DENOM for out in tx["vout"]):
                return tx

    def run_test(self):
        controller = self.nodes[0]
        clients = [self.nodes[1], self.nodes[2]]
        assert controller.getblockcount() < H - 1, "the setup ran past H"
        # No quorum signs InstantSend locks here, and the miner would hold an unlocked transaction back: off.
        controller.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("Each client: two 1.00001 DFCN coins, 0.0013 DFCN and two 1000.01 DFCN candidates, paid by another wallet")
        wallets = []
        outputs = {}
        for client in clients:
            # A legacy wallet created after startup has no automatic backup, and its client stops mixing for that.
            client.createwallet(wallet_name="mixer", descriptors=True)
            wallet = client.get_wallet_rpc("mixer")
            wallets.append(wallet)
            for amount in (OLD_DENOM, OLD_DENOM, ORDINARY, CANDIDATE, CANDIDATE):
                outputs[wallet.getnewaddress()] = amount
        controller.sendmany("", outputs)
        self.generate(controller, H - controller.getblockcount())
        assert_equal(controller.getblockcount(), H)
        old_coins, free, locked = [], [], []
        for wallet in wallets:
            coins = wallet.listunspent(1)
            assert_equal(sorted(Decimal(str(u["amount"])) for u in coins), [ORDINARY, OLD_DENOM, OLD_DENOM, CANDIDATE, CANDIDATE])
            old_coins.append({outpoint(u) for u in coins if Decimal(str(u["amount"])) == OLD_DENOM})
            candidates = [u for u in coins if Decimal(str(u["amount"])) == CANDIDATE]
            free.append(outpoint(candidates[0]))
            locked.append(outpoint(candidates[1]))
            assert wallet.lockunspent(False, [{"txid": locked[-1][0], "vout": locked[-1][1]}])
        client_offsets = [client.debug_log_bytes() for client in clients]

        self.log.info("H <= tip < U: each client's old attempt makes its collateral from the 0.0013 DFCN, and no "
                      "denominations: the old target (10 DFCN) wants more, but a candidate is no funds to it")
        for client, wallet, offset in zip(clients, wallets, client_offsets):
            assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
            self.wait_logged(client, offset, rb"(MakeCollateralAmounts|CreateDenominated) -- Start")
            wallet.coinjoin("stop")
        self.sync_mempools()
        self.generate(controller, 1)
        for i, (client, wallet) in enumerate(zip(clients, wallets)):
            assert free[i] in {outpoint(u) for u in wallet.listunspent(1)}, "the free candidate was spent"
            assert controller.gettxout(*locked[i]) is not None, "the locked candidate was spent"
            assert all(u["confirmations"] > 0 for u in wallet.listunspent(0))
            log = self.logged_text(client, client_offsets[i])
            assert b"MakeCollateralAmounts -- Start" in log and b"CreateDenominated -- Start" not in log

        self.log.info("The first client starts a queue for 1.00001; the masternode it names opens a session for it")
        mn_offsets = {mn.proTxHash: mn.node.debug_log_bytes() for mn in self.mninfo}
        assert_equal(wallets[0].coinjoin("start"), "Mixing started successfully")
        self.wait_logged(clients[0], client_offsets[0], rb"StartNewQueue -- pending connection, nSessionDenom: %d " % OLD_BIT)
        protx = wallets[0].getcoinjoininfo()["sessions"][0]["protxhash"]
        mn = next(mn for mn in self.mninfo if mn.proTxHash == protx)
        opened = self.wait_logged(mn.node, mn_offsets[protx],
                                  rb"CreateNewSession -- new session created, nSessionID: (\d+)  nSessionDenom: (\d+) ")
        assert_equal(int(opened.group(2)), OLD_BIT)
        session_id = opened.group(1)
        assert_equal(self.wait_logged(clients[0], client_offsets[0], rb"Set nSessionID to (\d+)\.").group(1), session_id)

        self.log.info("The second client sees the queue and joins it at the same masternode, in the same session")
        self.wait_until(lambda: wallets[1].getcoinjoininfo()["queue_size"] > 0, timeout=30)
        assert_equal(wallets[1].coinjoin("start"), "Mixing started successfully")
        self.wait_logged(clients[1], client_offsets[1],
                         rb"JoinExistingQueue -- pending connection \(from queue\): nSessionDenom: %d " % OLD_BIT)
        assert_equal(wallets[1].getcoinjoininfo()["sessions"][0]["protxhash"], protx)
        joined = self.wait_logged(mn.node, mn_offsets[protx], rb"AddUserToExistingSession -- new user accepted, nSessionID: (\d+) ")
        assert_equal(joined.group(1), session_id)
        assert_equal(self.wait_logged(clients[1], client_offsets[1], rb"Set nSessionID to (\d+)\.").group(1), session_id)

        self.log.info("Two peers of that masternode: one at %d, one at %d (v23.0.0)", NEW_VERSION, OLD_VERSION)
        new_peer = mn.node.add_p2p_connection(P2PVersionedPeer(NEW_VERSION), uacomment="new-peer")
        old_peer = mn.node.add_p2p_connection(P2PVersionedPeer(OLD_VERSION), uacomment="old-peer")
        versions = {p["subver"]: p["version"] for p in mn.node.getpeerinfo() if "-peer)" in p["subver"]}
        assert_equal([v for s, v in versions.items() if "(new-peer)" in s], [NEW_VERSION])
        assert_equal([v for s, v in versions.items() if "(old-peer)" in s], [OLD_VERSION])

        self.log.info("With mocktime past the queue time the masternode starts the session, and the two wallets mix 1.00001")
        ready_offset = mn.node.debug_log_bytes()
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % OLD_BIT)
        self.wait_until(lambda: self.mix_in_mempool() is not None, timeout=60)
        mix = self.mix_in_mempool()
        for wallet in wallets:
            wallet.coinjoin("stop")
        spent = {(vin["txid"], vin["vout"]) for vin in mix["vin"]}
        assert spent <= old_coins[0] | old_coins[1], "the mix spent a coin other than the wallets' 1.00001 coins"
        assert spent & old_coins[0] and spent & old_coins[1], "the mix lacks one of the wallets"

        self.log.info("Both peers, the %d one and the %d one, are offered the old DSTX and get it", NEW_VERSION, OLD_VERSION)
        mix_hash = int(mix["txid"], 16)

        def offered():
            # The inventory trickle runs on the mocked clock: three seconds of it per check, which with this half
            # second and wait_until's own half second is about three each second, as sync_mempools gives.
            time.sleep(0.5)
            self.bump_mocktime(3)
            with p2p_lock:
                return all((MSG_DSTX, mix_hash) in peer.offered and mix_hash in peer.dstxes for peer in (new_peer, old_peer))
        self.wait_until(offered, timeout=60)
        with p2p_lock:
            for peer in (new_peer, old_peer):
                assert_equal(peer.dstxes[mix_hash].m_protxHash, int(protx, 16))
        self.generate(controller, 1)

        self.log.info("Each wallet's new coins count one round; neither candidate is touched, and the lock stays")
        for i, (client, wallet) in enumerate(zip(clients, wallets)):
            new = [u for u in wallet.listunspent(1) if u["txid"] == mix["txid"]]
            assert_equal(len(new), len(spent & old_coins[i]))
            for coin in new:
                assert_equal(Decimal(str(coin["amount"])), OLD_DENOM)
                assert_equal(coin["coinjoin_rounds"], 1)
            unspent = {outpoint(u) for u in wallet.listunspent(1)}
            assert free[i] in unspent, "the free candidate was spent"
            assert locked[i] in {outpoint(u) for u in wallet.listlockunspent()}, "the user's lock is gone"
            assert controller.gettxout(*locked[i]) is not None, "the locked candidate was spent"
            log = self.logged_text(client, client_offsets[i])
            assert b"StartNewDfcnScaleQueue" not in log and b"JoinExistingDfcnScaleQueue" not in log, \
                "a DFCN-scale attempt ran before U"


if __name__ == '__main__':
    P2PCoinJoinDfcnOldMixTest().main()
