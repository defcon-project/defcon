#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A DFCN-scale session accepted from U finishes on a tip that fell below U; the next attempt there is the old one,
and the old denomination creator spends no DFCN-scale coin (CJ4-B2d3b).

H is 400, U (cj4use) 420. Each of two client wallets holds a 100.001 and a 1000.01 DFCN candidate and two 0.0004 DFCN
collateral inputs, all paid by another wallet and mined below U-3, and an old target of 2 DFCN: with nothing else its
old path stops at "Not enough funds to mix.", so from U every attempt is the DFCN-scale one (CJ4-B2b3). On the tip
U-1 the clients are paired as in p2p_coinjoin_dfcn_bits.py: the first is started until the masternode it names takes
its DFCN-scale queue (100.001 or 1000.01, a bit chosen at random from its work), then the second once, whose attempt
joins that queue; each attempt waits until its masternode has handled the request, a request dropped unanswered
starts the first client again, and a joining walk that found its list busy starts the pairing again from a clean
state. Then every node invalidates the block at U-3, and the tip falls to U-4: with mocktime past the queue time the
session still mixes its amount, one transaction of the two candidates of that amount, mined on U-3 (r16/R10). On the
tip U-3 (the next block U-2) each client's next attempt is the old one and stops at "Not enough funds to mix.",
although its other candidate and its new coin are still DFCN-scale work (r16/R10). Then each receives 3 DFCN of
ordinary funds, mined on U-2: on that tip the old attempt makes old denominations from them alone, and neither the
other candidate nor the new coin is spent (r13/R5).

The collateral is a coin received early, not one the client makes: a collateral transaction the reorg put back into
the mempool would be refused by the masternode without an InstantSend lock (src/coinjoin/coinjoin.cpp:
IsCollateralValid). The second one serves after a masternode charged the first for a session the test discarded.
"""

from decimal import Decimal
import re

from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

H = 400
U = 420
NEVER = 2**31 - 1
CANDIDATES = {32: Decimal("100.001"), 64: Decimal("1000.01")}  # bits 5 and 6 (src/coinjoin/common.h)
COLLATERAL = Decimal("0.0004")  # src/coinjoin/common.h: GetMaxCollateralAmount(), a collateral input as it is
ORDINARY = Decimal("3")        # ordinary funds, from which the old path makes denominations for its target of 2 DFCN
QUEUE_TIMEOUT = 30             # src/coinjoin/coinjoin.h: COINJOIN_QUEUE_TIMEOUT
TRIES = 5                      # the first client's attempts, each a queue a masternode may drop unanswered
PAIRINGS = 3                   # pairings started again after the joining attempt's walk found its list busy
# a start logs "pending connection, nSessionDenom: N", a join "pending connection (from queue): nSessionDenom: N"
PENDING = (rb"(StartNewDfcnScaleQueue|JoinExistingDfcnScaleQueue|StartNewQueue|JoinExistingQueue) -- "
           rb"pending connection(?:, | \(from queue\): )nSessionDenom: (\d+) ")
ANSWER = rb"ProcessPoolStateUpdate -- (accepted|rejected) by Masternode"
STOPS = b"DoAutomaticDenominating -- Not enough funds to mix."
# the mixing wallet's own manager (a client's default wallet has one too): its height, read from each new tip
TIP = rb"\[mixer\] CCoinJoinClientManager::UpdatedBlockTip -- nCachedBlockHeight: %d\r?\n"


def outpoint(coin):
    return (coin["txid"], coin["vout"])


class P2PCoinJoinDfcnReorgTest(DashTestFramework):
    def set_test_params(self):
        heights = ["-testactivationheight=superblocksretired@%d" % H, "-testactivationheight=mn_rr@%d" % NEVER,
                   "-testactivationheight=v231@%d" % H, "-testactivationheight=cj4use@%d" % U]
        client = heights + ["-debug=coinjoin", "-coinjoinsessions=1", "-coinjoinamount=2"]
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
        Returns the attempt's kind and denomination, the answer ("accepted", "rejected", or None for a request the
        masternode dropped unanswered), the masternode and every node's log offset from before the attempt."""
        client, wallet = self.clients[i], self.wallets[i]
        offsets = [node.debug_log_bytes() for node in self.nodes]
        assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
        found = self.wait_logged(client, offsets[client.index], PENDING)
        kind, denom = found.group(1).decode(), int(found.group(2))
        assert kind.endswith("DfcnScaleQueue"), "an attempt from U was the old one: %s" % kind
        assert denom in CANDIDATES, denom
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
        self.log.debug("client %d: %s, denomination %d, %s", i + 1, kind, denom, answer or "dropped unanswered")
        return kind, denom, answer, mn, offsets

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
        """The first client is started until the masternode it names takes its DFCN-scale queue, then the second once:
        its attempt joins that queue. Returns the masternode and the session's denomination, or None when the joining
        attempt found no queue: its walk gives up at once when the list is locked (src/coinjoin/coinjoin.cpp:
        GetQueueItemAndTry), and the attempt starts a queue of its own."""
        for _ in range(TRIES):
            kind, denom, answer, mn, offsets = self.attempt(0)
            # with nothing open an attempt starts a queue, and nothing turns it away
            assert_equal(kind, "StartNewDfcnScaleQueue")
            assert answer != "rejected", "client 1's request was rejected with nothing open"
            if answer == "accepted":
                break
            self.discard(0)
            self.clear_queues()
        else:
            raise AssertionError("no DFCN-scale queue of client 1 was taken in %d tries" % TRIES)
        opened = self.wait_logged(mn.node, offsets[mn.node.index],
                                  rb"CreateNewSession -- new session created, nSessionID: (\d+)  nSessionDenom: (\d+) ")
        assert_equal(int(opened.group(2)), denom)
        session_id = opened.group(1)
        assert_equal(self.wait_logged(self.clients[0], offsets[self.clients[0].index],
                                      rb"Set nSessionID to (\d+)\.").group(1), session_id)
        # the second client holds that queue: logged, then in its list (the only queue there)
        self.wait_logged(self.clients[1], offsets[self.clients[1].index], rb"DSQUEUE -- new CoinJoin queue \(nDenom=%d, " % denom)
        self.wait_until(lambda: self.wallets[1].getcoinjoininfo()["queue_size"] > 0, timeout=30)

        kind, joined_denom, answer, joined_mn, offsets = self.attempt(1)
        if kind == "StartNewDfcnScaleQueue":
            walked = self.logged_text(self.clients[1], offsets[self.clients[1].index])
            assert re.search(rb"JoinExistingDfcnScaleQueue -- ", walked) is None, \
                "a DFCN-scale attempt of client 2 saw the open queue and did not join it"
            return None
        assert_equal((kind, joined_denom, answer), ("JoinExistingDfcnScaleQueue", denom, "accepted"))
        assert_equal(joined_mn.proTxHash, mn.proTxHash)
        joined = self.wait_logged(mn.node, offsets[mn.node.index], rb"AddUserToExistingSession -- new user accepted, nSessionID: (\d+) ")
        assert_equal(joined.group(1), session_id)
        assert_equal(self.wait_logged(self.clients[1], offsets[self.clients[1].index],
                                      rb"Set nSessionID to (\d+)\.").group(1), session_id)
        return mn, denom

    def mix_in_mempool(self, amount):
        for txid in self.nodes[0].getrawmempool():
            tx = self.nodes[0].getrawtransaction(txid, True)
            if len(tx["vout"]) == 2 and all(Decimal(str(out["value"])) == amount for out in tx["vout"]):
                return tx
        return None

    def mine_one(self, height):
        """Mines one block, at `height`, and waits until each client's mixing manager has read it (its last mix's block
        and the next attempt's height read from it)."""
        offsets = [client.debug_log_bytes() for client in self.clients]
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].getblockcount(), height)
        for client, offset in zip(self.clients, offsets):
            self.wait_logged(client, offset, TIP % height)

    def run_test(self):
        controller = self.nodes[0]
        self.clients = [self.nodes[1], self.nodes[2]]
        assert controller.getblockcount() < U - 4, "the setup ran past U"
        # No quorum signs InstantSend locks here, and the miner would hold an unlocked transaction back: off.
        controller.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        self.flushers = {node.index: node.add_p2p_connection(P2PInterface()) for node in self.nodes}

        self.log.info("Each client: a 100.001 and a 1000.01 DFCN candidate and two 0.0004 DFCN collateral inputs, paid by "
                      "another wallet")
        self.wallets = []
        outputs = {}
        for client in self.clients:
            # A legacy wallet created after startup has no automatic backup, and its client stops mixing for that.
            client.createwallet(wallet_name="mixer", descriptors=True)
            wallet = client.get_wallet_rpc("mixer")
            self.wallets.append(wallet)
            for amount in sorted(CANDIDATES.values()) + [COLLATERAL, COLLATERAL]:
                outputs[wallet.getnewaddress()] = amount
        controller.sendmany("", outputs)
        self.generate(controller, 1)
        assert controller.getblockcount() < U - 3, "the reorg would undo the funding"
        self.generate(controller, U - 1 - controller.getblockcount())
        assert_equal(controller.getblockcount(), U - 1)
        coins = []
        for wallet in self.wallets:
            unspent = wallet.listunspent(1)
            # exact: no fee taken from any
            assert_equal(sorted(Decimal(str(u["amount"])) for u in unspent), sorted(list(CANDIDATES.values()) + [COLLATERAL] * 2))
            coins.append({Decimal(str(u["amount"])): outpoint(u) for u in unspent if Decimal(str(u["amount"])) != COLLATERAL})

        self.log.info("Tip U-1: the first client starts a DFCN-scale queue at the masternode it names, which opens a "
                      "session for it; the second client joins it there, in the same session")
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
            raise AssertionError("the second client never joined the open DFCN-scale queue in %d pairings" % PAIRINGS)
        mn, denom = paired
        amount = CANDIDATES[denom]
        other = next(a for d, a in CANDIDATES.items() if d != denom)
        self.log.info("The session is for %s DFCN, accepted on the tip %d", amount, controller.getblockcount())
        assert controller.getblockcount() >= U - 1

        self.log.info("Every node invalidates the block at U-3: the tip falls to U-4, below U, with the session open")
        client_offsets = [client.debug_log_bytes() for client in self.clients]
        fork = controller.getblockhash(U - 3)
        for node in self.nodes:
            node.invalidateblock(fork)
        self.sync_blocks()
        assert_equal(controller.getblockcount(), U - 4)
        for client, offset in zip(self.clients, client_offsets):
            self.wait_logged(client, offset, TIP % (U - 4))
        for wallet, own in zip(self.wallets, coins):
            mined = {outpoint(u) for u in wallet.listunspent(1)}
            assert set(own.values()) <= mined, "the reorg undid a candidate"

        self.log.info("With mocktime past the queue time the session still mixes its %s DFCN, on the tip U-4", amount)
        ready_offset = mn.node.debug_log_bytes()
        client_offsets = [client.debug_log_bytes() for client in self.clients]
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % denom)
        self.wait_until(lambda: self.mix_in_mempool(amount) is not None, timeout=60)
        mix = self.mix_in_mempool(amount)
        for wallet in self.wallets:
            wallet.coinjoin("stop")
        assert_equal({(vin["txid"], vin["vout"]) for vin in mix["vin"]}, {own[amount] for own in coins})
        # Each client counts the mix as its last success on the tip U-4, before the next block: an attempt waits for one
        # block after it (src/coinjoin/client.cpp: WaitForAnotherBlock).
        for client, offset in zip(self.clients, client_offsets):
            self.wait_logged(client, offset, rb"CompletedTransaction -- success\r?\n")
        assert_equal(controller.getblockcount(), U - 4)
        self.mine_one(U - 3)
        assert mix["txid"] in controller.getblock(controller.getbestblockhash())["tx"]
        new_coins = []
        for wallet in self.wallets:
            new = [u for u in wallet.listunspent(1) if u["txid"] == mix["txid"]]
            assert_equal(len(new), 1)
            assert_equal(Decimal(str(new[0]["amount"])), amount)
            assert_equal(new[0]["coinjoin_rounds"], 1)
            new_coins.append(outpoint(new[0]))

        self.log.info("Tip U-3 (the next block U-2): each client's next attempt is the old one, which stops, although its "
                      "%s DFCN candidate and its new coin are still DFCN-scale work", other)
        for client, wallet in zip(self.clients, self.wallets):
            offset = client.debug_log_bytes()
            wallet.coinjoin("start")  # runs the attempt before it answers (with the attempt's status: it stops)
            wallet.coinjoin("stop")
            log = self.logged_text(client, offset)
            assert b"DfcnScaleQueue" not in log and b"MakeCollateralAmounts" not in log, \
                "a DFCN-scale attempt ran on the tip U-3"
            self.wait_logged(client, offset, re.escape(STOPS))

        self.log.info("Each client receives 3 DFCN of ordinary funds, mined on U-2; on that tip (the next block U-1) the "
                      "old attempt makes old denominations from them alone")
        ordinary_addresses = [wallet.getnewaddress() for wallet in self.wallets]
        funding = controller.sendmany("", {address: ORDINARY for address in ordinary_addresses})
        self.mine_one(U - 2)
        ordinary = []
        for wallet, address in zip(self.wallets, ordinary_addresses):
            ordinary.append(next(outpoint(u) for u in wallet.listunspent(1, 9999999, [address]) if u["txid"] == funding))
        for client, wallet in zip(self.clients, self.wallets):
            offset = client.debug_log_bytes()
            wallet.coinjoin("start")
            wallet.coinjoin("stop")
            assert b"DfcnScaleQueue" not in self.logged_text(client, offset), "a DFCN-scale attempt ran on the tip U-2"
            self.wait_logged(client, offset, rb"CreateDenominated -- Start")
        self.sync_mempools()
        self.generate(controller, 1)
        block = controller.getblock(controller.getbestblockhash(), 2)
        for i in range(2):
            for coin in (coins[i][other], new_coins[i]):
                assert controller.gettxout(*coin) is not None, "a DFCN-scale coin was spent by the old creator"
            makers = [tx for tx in block["tx"] if any((vin.get("txid"), vin.get("vout")) == ordinary[i] for vin in tx["vin"])]
            assert_equal(len(makers), 1)
            assert_equal([(vin["txid"], vin["vout"]) for vin in makers[0]["vin"]], [ordinary[i]])
        self.log.info("Neither client's %s DFCN candidate nor its new coin was spent", other)


if __name__ == '__main__':
    P2PCoinJoinDfcnReorgTest().main()
