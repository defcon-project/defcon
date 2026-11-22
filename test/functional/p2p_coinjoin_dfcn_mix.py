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

The DSTX by protocol version (CJ4-B2d1): two peers of the mixing masternode, one at 70243 and one at 70242 (what
v23.0.0 advertises). The 70243 peer is offered the mix as a DSTX and gets it; the 70242 peer is offered neither the
DSTX nor the transaction, while it is offered an ordinary transaction sent after the mix, and once the mix may be
asked for unannounced, a request for it as a DSTX is answered as not found and one as a transaction gets it.
"""

from decimal import Decimal
import re
import time

from test_framework.messages import CInv, MSG_DSTX, MSG_TX, msg_getdata
from test_framework.p2p import P2PVersionedPeer, p2p_lock
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

H = 400
U = 410
NEVER = 2**31 - 1
NEW_BIT = 32                   # 100.001 DFCN
CANDIDATE = Decimal("100.001")
ORDINARY = Decimal("0.0013")   # less than the old path's minimum without collateral: it stops at "Not enough funds"
QUEUE_TIMEOUT = 30             # src/coinjoin/coinjoin.h: COINJOIN_QUEUE_TIMEOUT (a client waits 10 s longer)
OLD_VERSION = 70242            # src/version.h: Q60_SWITCHOVER_PROTO_VERSION, what v23.0.0 advertises
NEW_VERSION = 70243            # src/version.h: COINJOIN_DFCN_DENOMS_PROTO_VERSION
UNCONDITIONAL_RELAY_DELAY = 2 * 60  # src/net_processing.cpp: a transaction this old may be asked for unannounced


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

        self.log.info("Two peers of that masternode: one at %d, one at %d (v23.0.0)", NEW_VERSION, OLD_VERSION)
        new_peer = mn.node.add_p2p_connection(P2PVersionedPeer(NEW_VERSION), uacomment="new-peer")
        old_peer = mn.node.add_p2p_connection(P2PVersionedPeer(OLD_VERSION), uacomment="old-peer")
        # each test connection's own version, as the masternode recorded it
        versions = {p["subver"]: p["version"] for p in mn.node.getpeerinfo() if "-peer)" in p["subver"]}
        assert_equal(sorted(versions.values()), [OLD_VERSION, NEW_VERSION])
        assert_equal([v for s, v in versions.items() if "(new-peer)" in s], [NEW_VERSION])
        assert_equal([v for s, v in versions.items() if "(old-peer)" in s], [OLD_VERSION])

        self.log.info("With mocktime past the queue time the masternode starts the session, and the two wallets mix")
        ready_offset = mn.node.debug_log_bytes()
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % NEW_BIT)
        self.wait_until(lambda: self.mix_in_mempool() is not None, timeout=60)
        mix = self.mix_in_mempool()
        assert_equal({(vin["txid"], vin["vout"]) for vin in mix["vin"]}, {(c["txid"], c["vout"]) for c in candidates})
        for wallet in wallets:
            wallet.coinjoin("stop")

        self.log.info("The %d peer is offered the mix as a DSTX and gets it; the %d peer is offered nothing of it",
                      NEW_VERSION, OLD_VERSION)
        mix_hash = int(mix["txid"], 16)
        assert mix["txid"] in mn.node.getrawmempool()
        # An ordinary transaction after the mix: once the 70242 peer is offered it, the masternode's inventory round
        # that held the mix for that peer is over.
        marker_hash = int(controller.sendtoaddress(controller.getnewaddress(), 1), 16)

        def offered():
            # The masternode offers its peers its inventory at once (on a masternode fSendTrickle is always set,
            # src/net_processing.cpp: SendMessages); the clock is for the marker, which reaches the masternode through
            # nodes whose inventory trickle runs on the mocked clock: three seconds of it per check, which with this
            # half second and wait_until's own half second is about three each second, as sync_mempools gives.
            time.sleep(0.5)
            self.bump_mocktime(3)
            with p2p_lock:
                return ((MSG_DSTX, mix_hash) in new_peer.offered and mix_hash in new_peer.dstxes and
                        (MSG_TX, marker_hash) in old_peer.offered)
        self.wait_until(offered, timeout=60)
        old_peer.sync_with_ping()
        with p2p_lock:
            assert (MSG_DSTX, mix_hash) not in old_peer.offered, "the 70242 peer was offered the DSTX"
            assert (MSG_TX, mix_hash) not in old_peer.offered, "the 70242 peer was offered the mix as a transaction"
            assert mix_hash not in old_peer.dstxes and mix_hash not in old_peer.txids
            assert_equal(new_peer.dstxes[mix_hash].m_protxHash, int(protx, 16))

        self.log.info("Once the mix may be asked for unannounced, the %d peer's request for it as a DSTX is not found, "
                      "and one as a transaction gets it", OLD_VERSION)
        self.bump_mocktime(UNCONDITIONAL_RELAY_DELAY + 1)
        # Each request's answer on its own, between the request and its ping: about the mix, the DSTX request gets
        # exactly one not-found item and the transaction request exactly the transaction (as
        # src/test/coinjoin_dstx_relay_tests.cpp checks per request).
        with p2p_lock:
            start = len(old_peer.received)
        old_peer.send_and_ping(msg_getdata([CInv(MSG_DSTX, mix_hash)]))
        with p2p_lock:
            got = old_peer.received_since(start, mix_hash)
            start = len(old_peer.received)
        assert ("dstx", MSG_DSTX, mix_hash) not in got, "the 70242 peer was sent the DSTX"
        assert got == [("notfound", MSG_DSTX, mix_hash)], "the 70242 peer's DSTX request got %r, not one not-found item" % got
        old_peer.send_and_ping(msg_getdata([CInv(MSG_TX, mix_hash)]))
        with p2p_lock:
            got = old_peer.received_since(start, mix_hash)
        assert ("dstx", MSG_DSTX, mix_hash) not in got, "the 70242 peer was sent the DSTX for its transaction request"
        assert got == [("tx", MSG_TX, mix_hash)], "the 70242 peer's transaction request got %r, not the transaction alone" % got
        assert old_peer.is_connected and new_peer.is_connected
        self.generate(controller, 1)

        self.log.info("Each wallet's new coin counts one round")
        for wallet in wallets:
            new = [u for u in wallet.listunspent(1) if u["txid"] == mix["txid"]]
            assert_equal(len(new), 1)
            assert_equal(Decimal(str(new[0]["amount"])), CANDIDATE)
            assert_equal(new[0]["coinjoin_rounds"], 1)


if __name__ == '__main__':
    P2PCoinJoinDfcnMixTest().main()
