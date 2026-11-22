#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Two wallets with different old denominations and one common DFCN-scale one mix the common one (CJ4-B2d2b).

H is 400, U (cj4use) 410. One client wallet holds two coins of exactly 10.0001 DFCN (bit 1), the other two of
exactly 0.100001 DFCN (bit 4); each also holds a 100.001 DFCN candidate (bit 32) and 0.0013 DFCN of ordinary funds,
all paid by another wallet, and an old target of 2 DFCN. Their old queues can never pair, so wallets that only ever
tried their old bit would wait for a partner that never comes (r13/R6). From U each attempt is the old or the
DFCN-scale one by a coin toss (CJ4-B2b3), and coinjoin start runs one attempt at once, so the test drives them one at
a time: a client is started until an attempt of it starts a bit-5 queue, then the other until an attempt of it joins
that queue. An old attempt on the way names the client's own old bit, never the other's. Each attempt waits until its
masternode has handled the request (its answer, or a request it dropped unanswered, which opens nothing); a discarded
attempt's client is stopped and reset, and while no bit-5 queue is open, mocktime past the queue time clears every
session and queue list. A DFCN-scale attempt of the second client that saw the open queue joins it; one whose queue
walk found its list busy starts the pairing again from a clean state. With mocktime past the queue time both mix: one
transaction, the two candidates in and two outputs of 100.001 DFCN out, mined, each new coin one round, and neither
wallet's old coins touched.
"""

from decimal import Decimal
import re

from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

H = 400
U = 410
NEVER = 2**31 - 1
NEW_BIT = 32                   # 100.001 DFCN (src/coinjoin/common.h: vecDenominationsByBit)
CANDIDATE = Decimal("100.001")
OLD = [(Decimal("10.0001"), 1), (Decimal("0.100001"), 4)]  # each client's old denomination and its bit
ORDINARY = Decimal("0.0013")   # less than the old path's minimum without collateral: no new denominations from it
QUEUE_TIMEOUT = 30             # src/coinjoin/coinjoin.h: COINJOIN_QUEUE_TIMEOUT
TRIES = 20                     # each attempt is the DFCN-scale one with one chance in two
PAIRINGS = 3                   # pairings started again after the joining attempt's walk found its list busy
OLD_KINDS = ("StartNewQueue", "JoinExistingQueue")
# a start logs "pending connection, nSessionDenom: N", a join "pending connection (from queue): nSessionDenom: N"
PENDING = (rb"(StartNewDfcnScaleQueue|JoinExistingDfcnScaleQueue|StartNewQueue|JoinExistingQueue) -- "
           rb"pending connection(?:, | \(from queue\): )nSessionDenom: (\d+) ")
ANSWER = rb"ProcessPoolStateUpdate -- (accepted|rejected) by Masternode"


def outpoint(coin):
    return (coin["txid"], coin["vout"])


class P2PCoinJoinDfcnBitsTest(DashTestFramework):
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
        Returns the attempt's kind, the answer ("accepted", "rejected", or None for a request the masternode dropped
        unanswered), the masternode and every node's log offset from before the attempt."""
        client, wallet = self.clients[i], self.wallets[i]
        offsets = [node.debug_log_bytes() for node in self.nodes]
        assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
        found = self.wait_logged(client, offsets[client.index], PENDING)
        kind, denom = found.group(1).decode(), int(found.group(2))
        if kind in OLD_KINDS:
            assert_equal(denom, OLD[i][1])  # its own old bit, never the other's
        else:
            assert_equal(denom, NEW_BIT)
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
        block for a collateral a masternode charged. A session of two goes ready at the first step and times out at the
        second; its ready queue, stamped after the state change and so possibly after the second step, ages out at a
        third (the masternode is idle only once that queue is made). Every node finishes the
        message in hand before the lists count: a queue whose time check passed before the clock moved would come in
        after an empty list, and turn away the next queue of its masternode."""
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

    def pair(self, old_attempts):
        """A client is started until an attempt of it starts a bit-5 queue, then the other until an attempt of it joins
        that queue. Returns the masternode and the session id, or None when the joining client's DFCN-scale attempt
        found no queue: its walk gives up at once when the list is locked (src/coinjoin/coinjoin.cpp:
        GetQueueItemAndTry), and the attempt starts a queue of its own."""
        first = None
        for n in range(TRIES):
            i = n % 2
            kind, answer, mn, offsets = self.attempt(i)
            if kind == "StartNewDfcnScaleQueue" and answer == "accepted":
                first = i
                break
            # with nothing open an attempt starts a queue, and nothing turns it away
            assert kind in ("StartNewQueue", "StartNewDfcnScaleQueue"), kind
            assert answer != "rejected", "client %d's request was rejected with nothing open" % (i + 1)
            if kind == "StartNewQueue":
                old_attempts[i] += 1
            self.discard(i)
            self.clear_queues()
        assert first is not None, "no attempt started a DFCN-scale queue in %d tries" % TRIES
        second = 1 - first
        opened = self.wait_logged(mn.node, offsets[mn.node.index],
                                  rb"CreateNewSession -- new session created, nSessionID: (\d+)  nSessionDenom: (\d+) ")
        assert_equal(int(opened.group(2)), NEW_BIT)
        session_id = opened.group(1)
        assert_equal(self.wait_logged(self.clients[first], offsets[self.clients[first].index],
                                      rb"Set nSessionID to (\d+)\.").group(1), session_id)
        # the other client holds that queue: logged, then in its list (the only queue there)
        self.wait_logged(self.clients[second], offsets[self.clients[second].index],
                         rb"DSQUEUE -- new CoinJoin queue \(nDenom=%d, " % NEW_BIT)
        self.wait_until(lambda: self.wallets[second].getcoinjoininfo()["queue_size"] > 0, timeout=30)

        for _ in range(TRIES):
            kind, answer, joined_mn, offsets = self.attempt(second)
            if kind == "JoinExistingDfcnScaleQueue":
                assert_equal(answer, "accepted")
                break
            if kind == "StartNewDfcnScaleQueue":
                walked = self.logged_text(self.clients[second], offsets[self.clients[second].index])
                assert re.search(rb"JoinExistingDfcnScaleQueue -- ", walked) is None, \
                    "a DFCN-scale attempt of client %d saw the open bit-5 queue and did not join it" % (second + 1)
                return None
            old_attempts[second] += 1
            self.discard(second)
        else:
            raise AssertionError("no attempt joined the DFCN-scale queue in %d tries" % TRIES)
        assert_equal(joined_mn.proTxHash, mn.proTxHash)
        joined = self.wait_logged(mn.node, offsets[mn.node.index], rb"AddUserToExistingSession -- new user accepted, nSessionID: (\d+) ")
        assert_equal(joined.group(1), session_id)
        assert_equal(self.wait_logged(self.clients[second], offsets[self.clients[second].index],
                                      rb"Set nSessionID to (\d+)\.").group(1), session_id)
        return mn, session_id

    def mix_in_mempool(self):
        for txid in self.nodes[0].getrawmempool():
            tx = self.nodes[0].getrawtransaction(txid, True)
            if len(tx["vout"]) == 2 and all(Decimal(str(out["value"])) == CANDIDATE for out in tx["vout"]):
                return tx
        return None

    def run_test(self):
        controller = self.nodes[0]
        self.clients = [self.nodes[1], self.nodes[2]]
        assert controller.getblockcount() < U - 3, "the setup ran past U"
        # No quorum signs InstantSend locks here, and the miner would hold an unlocked transaction back: off.
        controller.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        self.flushers = {node.index: node.add_p2p_connection(P2PInterface()) for node in self.nodes}

        self.log.info("Two clients: two 10.0001 or two 0.100001 DFCN coins, each a 100.001 candidate and 0.0013 DFCN")
        self.wallets = []
        outputs = {}
        for client, (old, _) in zip(self.clients, OLD):
            # A legacy wallet created after startup has no automatic backup, and its client stops mixing for that.
            client.createwallet(wallet_name="mixer", descriptors=True)
            wallet = client.get_wallet_rpc("mixer")
            self.wallets.append(wallet)
            for amount in (old, old, CANDIDATE, ORDINARY):
                outputs[wallet.getnewaddress()] = amount
        controller.sendmany("", outputs)
        self.generate(controller, U - 1 - controller.getblockcount())
        assert_equal(controller.getblockcount(), U - 1)
        old_coins, candidates, ordinary = [], [], []
        for wallet, (old, _) in zip(self.wallets, OLD):
            coins = wallet.listunspent(1)
            assert_equal(sorted(Decimal(str(u["amount"])) for u in coins), sorted([old, old, CANDIDATE, ORDINARY]))
            old_coins.append({outpoint(u) for u in coins if Decimal(str(u["amount"])) == old})
            candidates.append(next(outpoint(u) for u in coins if Decimal(str(u["amount"])) == CANDIDATE))
            ordinary.append(next(outpoint(u) for u in coins if Decimal(str(u["amount"])) == ORDINARY))

        self.log.info("Tip U-1: each client's first attempt, old or DFCN-scale, makes its collateral from the 0.0013 "
                      "DFCN alone")
        for client, wallet in zip(self.clients, self.wallets):
            offset = client.debug_log_bytes()
            assert_equal(wallet.coinjoin("start"), "Mixing started successfully")
            self.wait_logged(client, offset, rb"MakeCollateralAmounts -- Start")
            wallet.coinjoin("stop")
        self.sync_mempools()
        self.generate(controller, 1)
        block = controller.getblock(controller.getbestblockhash(), 2)
        for i, wallet in enumerate(self.wallets):
            makers = [tx for tx in block["tx"] if any((vin.get("txid"), vin.get("vout")) == ordinary[i] for vin in tx["vin"])]
            assert_equal(len(makers), 1)
            assert_equal([(vin["txid"], vin["vout"]) for vin in makers[0]["vin"]], [ordinary[i]])
            assert all(u["confirmations"] > 0 for u in wallet.listunspent(0))

        self.log.info("A client is started until an attempt of it starts a bit-5 queue, then the other until an attempt "
                      "of it joins that queue; an old attempt is stopped and reset")
        old_attempts = [0, 0]
        for _ in range(PAIRINGS):
            paired = self.pair(old_attempts)
            if paired is not None:
                break
            self.log.info("The joining attempt found its queue list busy and started a queue of its own: both clients "
                          "are reset, and the pairing starts again from a clean state")
            for i in range(2):
                self.discard(i)
            self.clear_queues()
        else:
            raise AssertionError("the second client never joined the open bit-5 queue in %d pairings" % PAIRINGS)
        mn, _ = paired
        self.log.info("Old attempts on the way, each with the client's own old bit: client 1 %d, client 2 %d",
                      old_attempts[0], old_attempts[1])

        self.log.info("With mocktime past the queue time the two wallets mix their common DFCN-scale denomination")
        ready_offset = mn.node.debug_log_bytes()
        self.bump_mocktime(QUEUE_TIMEOUT + 1)
        self.wait_logged(mn.node, ready_offset, rb"queue is ready, signing and relaying \(nDenom=%d," % NEW_BIT)
        self.wait_until(lambda: self.mix_in_mempool() is not None, timeout=60)
        mix = self.mix_in_mempool()
        assert_equal({(vin["txid"], vin["vout"]) for vin in mix["vin"]}, set(candidates))
        for wallet in self.wallets:
            wallet.coinjoin("stop")
        self.generate(controller, 1)

        self.log.info("Each wallet's new coin counts one round, and its old coins are untouched")
        for i, wallet in enumerate(self.wallets):
            unspent = wallet.listunspent(1)
            new = [u for u in unspent if u["txid"] == mix["txid"]]
            assert_equal(len(new), 1)
            assert_equal(Decimal(str(new[0]["amount"])), CANDIDATE)
            assert_equal(new[0]["coinjoin_rounds"], 1)
            assert old_coins[i] <= {outpoint(u) for u in unspent}, "an old coin was spent"


if __name__ == '__main__':
    P2PCoinJoinDfcnBitsTest().main()
