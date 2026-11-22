#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""An old-amount mix between H and U while each wallet holds DFCN-scale candidates (CJ4-B2d2a), and a second old
session accepted before U that finishes above U (CJ4-B2d3a).

H is 400, U (cj4use) 430. Each of two client wallets holds two coins of exactly 1.00001 DFCN (an old denomination),
0.0013 DFCN of ordinary funds and two 1000.01 DFCN candidates, one of them locked by the user before mixing starts,
all paid by another wallet in two transactions, one 1.00001 coin in each, so that a mixing entry may take both (it
takes one input per parent transaction), and an old target of 10 DFCN. Between H and U the old path runs: each makes
its collateral from the 0.0013 alone and no denominations (a candidate is no funds to it, whatever its target); the first
starts a queue for 1.00001 at the masternode it names, the second joins it there, and with mocktime past the queue
time both mix: one transaction whose inputs are the wallets' 1.00001 coins, at least one of each, and whose outputs
are all 1.00001, mined, each new coin one round. Neither candidate is touched: both stay unspent, the user's lock
stays, and no DFCN-scale attempt runs. Two peers of the mixing masternode, one at 70243 and one at 70242 (what
v23.0.0 advertises), are both offered the old DSTX and get it.

Then, with every queue cleared, the clients are paired again for 1.00001, still below U, and blocks are mined up to U
while that session waits: with mocktime past the queue time it readies on the tip U, where a new attempt could be a
DFCN-scale one, and still mixes 1.00001 (note § 9.4, r17/R6): one transaction of the wallets' 1.00001 coins, mined,
neither candidate touched, the lock kept, and no DFCN-scale attempt run while the session waited.

Each attempt waits until its masternode has handled the request, as in p2p_coinjoin_dfcn_bits.py: a request for a new
session the masternode dropped unanswered opens nothing, and the first client is stopped, reset and started again
after mocktime past the queue time cleared every session and queue list; a joining attempt whose queue walk found its
list busy starts a queue of its own, and the pairing starts again from a clean state.
"""

from decimal import Decimal
import re

from test_framework.messages import MSG_DSTX
from test_framework.p2p import P2PInterface, P2PVersionedPeer, p2p_lock
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
TRIES = 5                      # the first client's attempts, each a queue a masternode may drop unanswered
PAIRINGS = 3                   # pairings started again after the joining attempt's walk found its list busy
# a start logs "pending connection, nSessionDenom: N", a join "pending connection (from queue): nSessionDenom: N"
PENDING = rb"(StartNewQueue|JoinExistingQueue) -- pending connection(?:, | \(from queue\): )nSessionDenom: (\d+) "
ANSWER = rb"ProcessPoolStateUpdate -- (accepted|rejected) by Masternode"


def outpoint(coin):
    return (coin["txid"], coin["vout"])


class P2PCoinJoinDfcnOldMixTest(DashTestFramework):
    def set_test_params(self):
        heights = ["-testactivationheight=superblocksretired@%d" % H, "-testactivationheight=mn_rr@%d" % NEVER,
                   "-testactivationheight=v231@%d" % H, "-testactivationheight=cj4use@%d" % U]
        # The old target, 10 DFCN, is more than the wallets' two 1.00001 coins: the old path would make denominations
        # from any funds it may use, and a 1000.01 candidate is none.
        client = heights + ["-debug=coinjoin", "-coinjoinsessions=1", "-coinjoinamount=10"]
        # node0 the controller, node1 and node2 the clients, node3-node5 the masternodes; a masternode logs the
        # messages it receives and sends (-debug=net), so the test sees whether it answered a request.
        self.set_dash_test_params(6, 3, [heights, client, client] + [heights + ["-debug=coinjoin", "-debug=net"]] * 3)

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

    def finish_in_hand(self, node):
        """Returns once the node has finished the message it had in hand: it handles one message at a time, and a ping
        on the test's own connection comes after it."""
        self.flushers[node.index].sync_with_ping()

    def attempt(self, i):
        """Starts client i, which runs one attempt at once, and waits until the masternode has handled its request.
        Returns the attempt's kind, the answer ("accepted", "rejected", or None for a request the masternode dropped
        unanswered), the masternode and every node's log offset from before the attempt."""
        client, wallet = self.clients[i], self.wallets[i]
        offsets = [node.debug_log_bytes() for node in self.nodes]
        assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
        found = self.wait_logged(client, offsets[client.index], PENDING)
        kind, denom = found.group(1).decode(), int(found.group(2))
        assert_equal(denom, OLD_BIT)
        protx = wallet.getcoinjoininfo()["sessions"][0]["protxhash"]
        mn = next(mn for mn in self.mninfo if mn.proTxHash == protx)
        mn_offset = offsets[mn.node.index]
        # The masternode receives the request, then is done with it; it answers with a status update to that peer,
        # but a request for a new session it drops unanswered when its queue lock is busy (src/coinjoin/server.cpp:
        # ProcessDSACCEPT), and it opens nothing then.
        peer = int(self.wait_logged(mn.node, mn_offset, rb"received: dsa \(\d+ bytes\) peer=(\d+)\r?\n").group(1))
        self.finish_in_hand(mn.node)
        if re.search(rb"sending dssu \(\d+ bytes\) peer=%d\r?\n" % peer, self.logged_text(mn.node, mn_offset)):
            answer = self.wait_logged(client, offsets[client.index], ANSWER).group(1).decode()
        else:
            answer = None
            assert_equal(mn.node.getcoinjoininfo()["state"], "IDLE")
        self.log.debug("client %d: %s, %s", i + 1, kind, answer or "dropped unanswered")
        return kind, answer, mn, offsets

    def discard(self, i):
        """Stops client i if it mixes, and resets it: the stop alone keeps the session of its attempt."""
        if self.wallets[i].getcoinjoininfo()["running"]:
            self.wallets[i].coinjoin("stop")
        assert_equal(self.wallets[i].coinjoin("reset"), "Mixing was reset")

    def mn_states(self):
        return [mn.node.getcoinjoininfo()["state"] for mn in self.mninfo]

    def clear_queues(self):
        """Mocktime past the queue time until every masternode is idle and every node's queue list is empty, then a
        block for a collateral a masternode charged (as p2p_coinjoin_dfcn_bits.py clears them)."""
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_until(lambda: all(state in ("IDLE", "ACCEPTING_ENTRIES") for state in self.mn_states()), timeout=30)
        if any(state != "IDLE" for state in self.mn_states()):
            self.bump_mocktime(QUEUE_TIMEOUT + 1)
            self.wait_until(lambda: all(state == "IDLE" for state in self.mn_states()), timeout=30)
            self.bump_mocktime(QUEUE_TIMEOUT + 1)
        for node in self.nodes:
            self.finish_in_hand(node)

        def cleared():
            return (all(mn.node.getcoinjoininfo()["queue_size"] == 0 for mn in self.mninfo) and
                    self.nodes[0].getcoinjoininfo()["queue_size"] == 0 and
                    all(wallet.getcoinjoininfo()["queue_size"] == 0 for wallet in self.wallets))
        self.wait_until(cleared, timeout=30)
        if any(node.getrawmempool() for node in self.nodes):
            self.sync_mempools()
            self.generate(self.nodes[0], 1)

    def pair(self):
        """The first client is started until the masternode it names takes its queue for 1.00001, then the second once:
        its attempt joins that queue. Returns the masternode and the session id, or None when the joining attempt found
        no queue: its walk gives up at once when the list is locked (src/coinjoin/coinjoin.cpp: GetQueueItemAndTry),
        and the attempt starts a queue of its own."""
        for _ in range(TRIES):
            kind, answer, mn, offsets = self.attempt(0)
            # with nothing open an attempt starts a queue, and nothing turns it away
            assert_equal(kind, "StartNewQueue")
            assert answer != "rejected", "client 1's request was rejected with nothing open"
            if answer == "accepted":
                break
            self.discard(0)
            self.clear_queues()
        else:
            raise AssertionError("no queue of client 1 was taken in %d tries" % TRIES)
        opened = self.wait_logged(mn.node, offsets[mn.node.index],
                                  rb"CreateNewSession -- new session created, nSessionID: (\d+)  nSessionDenom: (\d+) ")
        assert_equal(int(opened.group(2)), OLD_BIT)
        session_id = opened.group(1)
        assert_equal(self.wait_logged(self.clients[0], offsets[self.clients[0].index],
                                      rb"Set nSessionID to (\d+)\.").group(1), session_id)
        # the second client holds that queue: logged, then in its list (the only queue there)
        self.wait_logged(self.clients[1], offsets[self.clients[1].index], rb"DSQUEUE -- new CoinJoin queue \(nDenom=%d, " % OLD_BIT)
        self.wait_until(lambda: self.wallets[1].getcoinjoininfo()["queue_size"] > 0, timeout=30)

        kind, answer, joined_mn, offsets = self.attempt(1)
        if kind == "StartNewQueue":
            walked = self.logged_text(self.clients[1], offsets[self.clients[1].index])
            assert re.search(rb"JoinExistingQueue -- ", walked) is None, \
                "an attempt of client 2 saw the open queue and did not join it"
            return None
        assert_equal(answer, "accepted")
        assert_equal(joined_mn.proTxHash, mn.proTxHash)
        joined = self.wait_logged(mn.node, offsets[mn.node.index], rb"AddUserToExistingSession -- new user accepted, nSessionID: (\d+) ")
        assert_equal(joined.group(1), session_id)
        assert_equal(self.wait_logged(self.clients[1], offsets[self.clients[1].index],
                                      rb"Set nSessionID to (\d+)\.").group(1), session_id)
        return mn, session_id

    def wait_completed(self, offsets, session_id):
        """Waits until each client has logged the masternode's completion of the session `session_id` and its own
        success: it then counts the mix as its last success on the tip it holds, and an attempt waits for one block
        after that (src/coinjoin/client.cpp: WaitForAnotherBlock), so the block that mines the mix must come later."""
        for client, offset in zip(self.clients, offsets):
            done = self.wait_logged(client, offset, rb"DSCOMPLETE -- nMsgSessionID %s  nMsgMessageID \d+ " % session_id)
            self.wait_logged(client, offset + done.end(), rb"CompletedTransaction -- success\r?\n")

    def mix_in_mempool(self):
        for txid in self.nodes[0].getrawmempool():
            tx = self.nodes[0].getrawtransaction(txid, True)
            if len(tx["vin"]) == len(tx["vout"]) >= 2 and all(Decimal(str(out["value"])) == OLD_DENOM for out in tx["vout"]):
                return tx

    def run_test(self):
        controller = self.nodes[0]
        clients = self.clients = [self.nodes[1], self.nodes[2]]
        assert controller.getblockcount() < H - 1, "the setup ran past H"
        # No quorum signs InstantSend locks here, and the miner would hold an unlocked transaction back: off.
        controller.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        self.flushers = {node.index: node.add_p2p_connection(P2PInterface()) for node in self.nodes}

        self.log.info("Each client: two 1.00001 DFCN coins, 0.0013 DFCN and two 1000.01 DFCN candidates, paid by another wallet")
        wallets = self.wallets = []
        outputs = {}
        for client in clients:
            # A legacy wallet created after startup has no automatic backup, and its client stops mixing for that.
            client.createwallet(wallet_name="mixer", descriptors=True)
            wallet = client.get_wallet_rpc("mixer")
            wallets.append(wallet)
            for amount in (OLD_DENOM, ORDINARY, CANDIDATE, CANDIDATE):
                outputs[wallet.getnewaddress()] = amount
        controller.sendmany("", outputs)
        # the second 1.00001 coin of each in a transaction of its own
        controller.sendmany("", {wallet.getnewaddress(): OLD_DENOM for wallet in wallets})
        self.generate(controller, H - controller.getblockcount())
        assert_equal(controller.getblockcount(), H)
        old_coins, free, locked, ordinary = [], [], [], []
        for wallet in wallets:
            coins = wallet.listunspent(1)
            assert_equal(sorted(Decimal(str(u["amount"])) for u in coins), [ORDINARY, OLD_DENOM, OLD_DENOM, CANDIDATE, CANDIDATE])
            old_coins.append({outpoint(u) for u in coins if Decimal(str(u["amount"])) == OLD_DENOM})
            assert_equal(len({txid for txid, _ in old_coins[-1]}), 2)
            ordinary.append(next(outpoint(u) for u in coins if Decimal(str(u["amount"])) == ORDINARY))
            candidates = [u for u in coins if Decimal(str(u["amount"])) == CANDIDATE]
            free.append(outpoint(candidates[0]))
            locked.append(outpoint(candidates[1]))
            assert wallet.lockunspent(False, [{"txid": locked[-1][0], "vout": locked[-1][1]}])
        client_offsets = [client.debug_log_bytes() for client in clients]

        self.log.info("H <= tip < U: each client's old attempt makes its collateral from the 0.0013 DFCN alone, and no "
                      "denominations: the old target (10 DFCN) wants more, but a candidate is no funds to it")
        for client, wallet, offset in zip(clients, wallets, client_offsets):
            assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
            self.wait_logged(client, offset, rb"(MakeCollateralAmounts|CreateDenominated) -- Start")
            wallet.coinjoin("stop")
        self.sync_mempools()
        self.generate(controller, 1)
        block = controller.getblock(controller.getbestblockhash(), 2)
        for i, (client, wallet) in enumerate(zip(clients, wallets)):
            assert free[i] in {outpoint(u) for u in wallet.listunspent(1)}, "the free candidate was spent"
            makers = [tx for tx in block["tx"] if any((vin.get("txid"), vin.get("vout")) == ordinary[i] for vin in tx["vin"])]
            assert_equal(len(makers), 1)
            assert_equal([(vin["txid"], vin["vout"]) for vin in makers[0]["vin"]], [ordinary[i]])
            assert controller.gettxout(*locked[i]) is not None, "the locked candidate was spent"
            assert all(u["confirmations"] > 0 for u in wallet.listunspent(0))
            log = self.logged_text(client, client_offsets[i])
            assert b"MakeCollateralAmounts -- Start" in log and b"CreateDenominated -- Start" not in log

        self.log.info("The first client starts a queue for 1.00001 at the masternode it names, which opens a session for "
                      "it; the second client joins it there, in the same session")
        for _ in range(PAIRINGS):
            paired = self.pair()
            if paired is not None:
                break
            self.log.info("The joining attempt found its queue list busy and started a queue of its own: both clients "
                          "are reset, and the pairing starts again from a clean state")
            for i in range(2):
                self.discard(i)
            self.clear_queues()
        else:
            raise AssertionError("the second client never joined the open queue in %d pairings" % PAIRINGS)
        mn, session_id = paired
        protx = mn.proTxHash
        assert controller.getblockcount() < U, "the pairing ran past U"

        self.log.info("Two peers of that masternode: one at %d, one at %d (v23.0.0)", NEW_VERSION, OLD_VERSION)
        new_peer = mn.node.add_p2p_connection(P2PVersionedPeer(NEW_VERSION), uacomment="new-peer")
        old_peer = mn.node.add_p2p_connection(P2PVersionedPeer(OLD_VERSION), uacomment="old-peer")
        versions = {p["subver"]: p["version"] for p in mn.node.getpeerinfo() if "-peer)" in p["subver"]}
        assert_equal([v for s, v in versions.items() if "(new-peer)" in s], [NEW_VERSION])
        assert_equal([v for s, v in versions.items() if "(old-peer)" in s], [OLD_VERSION])

        self.log.info("With mocktime past the queue time the masternode starts the session, and the two wallets mix 1.00001")
        ready_offset = mn.node.debug_log_bytes()
        completion_offsets = [client.debug_log_bytes() for client in clients]
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % OLD_BIT)
        self.wait_until(lambda: self.mix_in_mempool() is not None, timeout=60)
        mix = self.mix_in_mempool()
        for wallet in wallets:
            wallet.coinjoin("stop")
        spent = {(vin["txid"], vin["vout"]) for vin in mix["vin"]}
        assert spent <= old_coins[0] | old_coins[1], "the mix spent a coin other than the wallets' 1.00001 coins"
        assert spent & old_coins[0] and spent & old_coins[1], "the mix lacks one of the wallets"
        self.log.info("The mix takes %d of the first wallet's 1.00001 coins and %d of the second's",
                      len(spent & old_coins[0]), len(spent & old_coins[1]))

        self.log.info("Both peers, the %d one and the %d one, are offered the old DSTX and get it", NEW_VERSION, OLD_VERSION)
        mix_hash = int(mix["txid"], 16)

        def offered():
            # A masternode offers its peers its inventory at once (on a masternode fSendTrickle is always set,
            # src/net_processing.cpp: SendMessages), so no clock is moved for it.
            with p2p_lock:
                return all((MSG_DSTX, mix_hash) in peer.offered and mix_hash in peer.dstxes for peer in (new_peer, old_peer))
        self.wait_until(offered, timeout=60)
        with p2p_lock:
            for peer in (new_peer, old_peer):
                assert_equal(peer.dstxes[mix_hash].m_protxHash, int(protx, 16))
        self.wait_completed(completion_offsets, session_id)
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

        self.log.info("A second old session, accepted before U, readies and finishes above U: the clients are paired "
                      "again, then blocks are mined up to U while it waits")
        self.clear_queues()
        client_offsets = [client.debug_log_bytes() for client in clients]
        old_now = [{outpoint(u) for u in wallet.listunspent(1) if Decimal(str(u["amount"])) == OLD_DENOM} for wallet in wallets]
        for _ in range(PAIRINGS):
            paired = self.pair()
            if paired is not None:
                break
            self.log.info("The joining attempt found its queue list busy and started a queue of its own: both clients "
                          "are reset, and the pairing starts again from a clean state")
            for i in range(2):
                self.discard(i)
            self.clear_queues()
        else:
            raise AssertionError("the second client never joined the second open queue in %d pairings" % PAIRINGS)
        mn, session_id = paired
        # accepted where the client's gate is closed (the next block below U): the attempts were old by the height
        assert controller.getblockcount() < U - 1, "the second session was accepted on an open gate"
        self.generate(controller, U - controller.getblockcount())
        assert_equal(controller.getblockcount(), U)
        assert_equal(mn.node.getcoinjoininfo()["state"], "QUEUE")  # the session waits, its two users in it

        self.log.info("Tip U: with mocktime past the queue time the old session readies and the two wallets mix 1.00001")
        ready_offset = mn.node.debug_log_bytes()
        completion_offsets = [client.debug_log_bytes() for client in clients]
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % OLD_BIT)
        self.wait_until(lambda: self.mix_in_mempool() is not None, timeout=60)
        second = self.mix_in_mempool()
        for wallet in wallets:
            wallet.coinjoin("stop")
        spent = {(vin["txid"], vin["vout"]) for vin in second["vin"]}
        assert spent <= old_now[0] | old_now[1], "the mix above U spent a coin other than the wallets' 1.00001 coins"
        assert spent & old_now[0] and spent & old_now[1], "the mix above U lacks one of the wallets"
        # both clients finish the session accepted before U (its completion and their success), above U
        self.wait_completed(completion_offsets, session_id)
        self.generate(controller, 1)
        assert second["txid"] in controller.getblock(controller.getbestblockhash())["tx"]
        self.log.info("The mix above U takes %d of the first wallet's 1.00001 coins and %d of the second's; neither "
                      "candidate is touched, the lock stays, and no DFCN-scale attempt ran while the session waited",
                      len(spent & old_now[0]), len(spent & old_now[1]))
        for i, (client, wallet) in enumerate(zip(clients, wallets)):
            assert free[i] in {outpoint(u) for u in wallet.listunspent(1)}, "the free candidate was spent above U"
            assert locked[i] in {outpoint(u) for u in wallet.listlockunspent()}, "the user's lock is gone"
            assert controller.gettxout(*locked[i]) is not None, "the locked candidate was spent above U"
            log = self.logged_text(client, client_offsets[i])
            assert b"StartNewDfcnScaleQueue" not in log and b"JoinExistingDfcnScaleQueue" not in log, \
                "a DFCN-scale attempt ran while the old session waited"


if __name__ == '__main__':
    P2PCoinJoinDfcnOldMixTest().main()
