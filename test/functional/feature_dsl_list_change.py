#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The Sentinel tick speaks from the epoch base's list when the list changes mid-epoch.

The receivers and the aggregation judge every announcement and report against
the masternode list of the epoch's base block. A tick that worked from the
chain tip's list instead would, after a mid-epoch list change, report on a
masternode the epoch does not know (refused everywhere else, but pooled at
home), or fall silent where the epoch still expects it to speak. Two changes
are made here, each in an epoch preceded by an intact one whose report count
it has to match:

- a masternode registered mid-epoch: nobody reports on it and it reports on
  nobody, no node refuses an announcement or a report, and every pool holds
  the same reports. It neither announces for the epoch nor answers a challenge
  (POSECHALLENGE) for it. It speaks, and is watched, from the next epoch on;
- a masternode revoked mid-epoch (ProUpRevTx: operator key reset, banned out of
  the valid set at the tip): it keeps reporting for the epoch with the key the
  base list holds, so the epoch pools as many reports as the intact one did,
  and it still answers a challenge for the epoch.

Everything observed here is the off-chain pool; no consensus rule is exercised.
"""

import os
import re
import struct

from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync

EPOCH_INTERVAL = 24
CUTOFF = EPOCH_INTERVAL - EPOCH_INTERVAL // 4  # reports are emitted from this offset

# Every refusal of an announcement or a report names its reason under the dsl
# log category (CPoSeServiceManager::ProcessResponse, CServiceReportStore::AddReport).
REFUSAL = re.compile(r"DSL -- (?:report by \S+ on \S+|announcement by \S+) for epoch \d+ refused: [^\n]*")


class msg_poseresp:
    """CPoSeServiceResponse, read only: nEpoch, proTxHash, then the signature."""
    msgtype = b"poseresp"

    def __init__(self):
        self.nEpoch = 0
        self.proTxHash = 0

    def deserialize(self, f):
        self.nEpoch = struct.unpack("<I", f.read(4))[0]
        self.proTxHash = int.from_bytes(f.read(32), "little")
        f.read()

    def __repr__(self):
        return "msg_poseresp(nEpoch=%d, proTxHash=%064x)" % (self.nEpoch, self.proTxHash)


class msg_dsl_flood:
    """A flooded Sentinel message this test only lets pass."""
    def deserialize(self, f):
        f.read()

    def __repr__(self):
        return "msg_%s()" % self.msgtype.decode()


class msg_posereport(msg_dsl_flood):
    msgtype = b"posereport"


class msg_posecommit(msg_dsl_flood):
    msgtype = b"posecommit"


class msg_posechal:
    """CPoSeServiceChallenge: nEpoch alone."""
    msgtype = b"posechal"

    def __init__(self, nEpoch):
        self.nEpoch = nEpoch

    def serialize(self):
        return struct.pack("<I", self.nEpoch)

    def __repr__(self):
        return "msg_posechal(nEpoch=%d)" % self.nEpoch


MESSAGEMAP[b"poseresp"] = msg_poseresp
MESSAGEMAP[b"posereport"] = msg_posereport
MESSAGEMAP[b"posecommit"] = msg_posecommit


class DSLListener(P2PInterface):
    """Records every liveness announcement a node sends it, as (epoch, proTxHash)."""
    def __init__(self):
        super().__init__()
        self.announcements = set()

    def on_poseresp(self, message):
        self.announcements.add((message.nEpoch, message.proTxHash))

    def on_posereport(self, message):
        pass

    def on_posecommit(self, message):
        pass


class DSLListChangeTest(DashTestFramework):
    def set_test_params(self):
        # Seven masternodes, then eight: never more than nDSLSentinelCount (7)
        # + 1, so every valid masternode watches every other one and an intact
        # epoch pools n * (n - 1) reports. A list change that moved who speaks
        # moves that count.
        self.set_dash_test_params(8, 7, extra_args=[["-testactivationheight=dsl@1"]] * 8)

    def running(self):
        return [n for n in self.nodes if n.running]

    def log_offsets(self):
        return {n.index: os.path.getsize(n.debug_log_path) for n in self.running()}

    def log_since(self, node, offsets):
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(offsets.get(node.index, 0))
            return f.read()

    def assert_no_refusals(self, offsets):
        for n in self.running():
            refusals = REFUSAL.findall(self.log_since(n, offsets))
            assert_equal((n.index, refusals), (n.index, []))

    def enter_next_epoch(self):
        node = self.nodes[0]
        height = node.getblockcount()
        self.bump_mocktime(60)
        self.generate(node, EPOCH_INTERVAL - height % EPOCH_INTERVAL)
        return node.getblockcount() // EPOCH_INTERVAL

    def walk_to_cutoff(self):
        node = self.nodes[0]
        pos = node.getblockcount() % EPOCH_INTERVAL
        assert pos < CUTOFF, f"already past the cutoff (position {pos})"
        self.bump_mocktime(30)
        self.generate(node, CUTOFF - pos)

    def wait_announcements(self, epoch, count):
        def flooded():
            for n in self.running():
                status = n.dslstatus()
                if status["epoch"] != epoch or status["respondedcount"] != count:
                    return False
            return True
        self.wait_until(flooded, timeout=60)

    def wait_pool(self, epoch, count):
        """Wait until every running node pools `count` reports for `epoch`, the
        same ones everywhere, none of them MISSED, and would aggregate them into
        a verdict that judges every masternode of the epoch and marks none
        MISSED; return the pool digest."""
        last = {}

        def pooled():
            for n in self.running():
                status = n.dslstatus()
                candidate = status["candidate"]
                last[n.index] = (status["epoch"], status["epochreports"], status["missedreports"],
                                 candidate["missedcount"], candidate["unobservedcount"], status["poolhash"])
            return all(s[:5] == (epoch, count, 0, 0, 0) for s in last.values()) and \
                len({s[5] for s in last.values()}) == 1

        try:
            self.wait_until(pooled, timeout=60)
        except AssertionError:
            for idx, s in sorted(last.items()):
                self.log.error("node %d: epoch %d, %d reports (%d missed), candidate missed %d unobserved %d, pool %s",
                               idx, *s)
            raise
        return next(iter(last.values()))[5]

    def revoke(self, mn):
        node = self.nodes[0]
        funds_address = node.getnewaddress()
        fund_txid = node.sendtoaddress(funds_address, 1)
        self.bump_mocktime(10 * 60 + 1)  # to make the tx safe to include in a block
        tip = self.generate(node, 1)[0]
        assert_equal(node.getrawtransaction(fund_txid, 1, tip)["confirmations"], 1)

        protx = node.protx("revoke", mn.proTxHash, mn.keyOperator, 1, funds_address)
        self.bump_mocktime(10 * 60 + 1)
        tip = self.generate(node, 1, sync_fun=self.no_op)[0]
        assert_equal(node.getrawtransaction(protx, 1, tip)["confirmations"], 1)

        def revoked():
            state = node.protx("info", mn.proTxHash)["state"]
            return state["PoSeBanHeight"] != -1 and state["revocationReason"] == 1
        self.wait_until(revoked)
        # The revocation resets the operator key, and a peer that knew the node
        # by it drops the connection (CMNAuth::NotifyMasternodeListChanged). The
        # drop is only marked there; the socket closes on the next pass of the
        # network thread, and an addnode before that finds the old connection
        # and opens none. So wait until neither side lists the other, then bring
        # the node back so its reports can flood. A new connection is not
        # dropped: the node has no masternode identity left to authenticate with.
        mn_node = self.nodes[mn.nodeIdx]
        mn_subver = mn_node.getnetworkinfo()["subversion"]
        node_subver = node.getnetworkinfo()["subversion"]
        self.wait_until(lambda: all(p["subver"] != mn_subver for p in node.getpeerinfo()) and
                        all(p["subver"] != node_subver for p in mn_node.getpeerinfo()))
        self.connect_nodes(mn.nodeIdx, 0)
        self.sync_blocks(self.running())

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)

        self.log.info("An intact epoch: every masternode reports on every other")
        epoch = self.enter_next_epoch()
        mns = len(self.mninfo)
        self.wait_announcements(epoch, mns)
        self.walk_to_cutoff()
        intact = mns * (mns - 1)
        self.wait_pool(epoch, intact)

        self.log.info("A masternode registered mid-epoch is no part of the epoch")
        epoch = self.enter_next_epoch()
        self.wait_announcements(epoch, mns)
        offsets = self.log_offsets()
        newcomer = self.dynamically_add_masternode()
        assert_equal(node.getblockcount() // EPOCH_INTERVAL, epoch)
        listener = self.nodes[newcomer.nodeIdx].add_p2p_connection(DSLListener())
        self.walk_to_cutoff()  # the blocks also earn the listener's connection its Sentinel message budget
        # confirmed by now: the tip's list counts it among the masternodes that
        # watch and are watched, while the base's list does not hold it at all
        tip_list = node.protx("diff", 1, node.getblockcount())["mnList"]
        assert int(next(e for e in tip_list if e["proRegTxHash"] == newcomer.proTxHash)["confirmedHash"], 16) != 0
        listener.send_and_ping(msg_posechal(epoch))  # an answer would come before the pong
        self.wait_pool(epoch, intact)
        self.assert_no_refusals(offsets)
        assert f"not on epoch {epoch}'s base list" in self.log_since(self.nodes[newcomer.nodeIdx], offsets)
        # neither announced nor re-announced on the challenge
        assert (epoch, int(newcomer.proTxHash, 16)) not in listener.announcements

        self.log.info("From the next epoch on it speaks and is watched")
        epoch = self.enter_next_epoch()
        mns = len(self.mninfo)
        assert_equal(mns, 8)
        self.wait_announcements(epoch, mns)
        self.walk_to_cutoff()
        intact = mns * (mns - 1)
        self.wait_pool(epoch, intact)

        self.log.info("A masternode revoked mid-epoch keeps reporting for the epoch")
        epoch = self.enter_next_epoch()
        self.wait_announcements(epoch, mns)
        offsets = self.log_offsets()
        revoked = self.mninfo[1]
        # connected after the epoch's announcements flooded: the only
        # announcement by the revoked node it can get is an answer to a challenge
        asker = self.nodes[revoked.nodeIdx].add_p2p_connection(DSLListener())
        self.revoke(revoked)  # the blocks also earn the asker's connection its Sentinel message budget
        assert_equal(node.getblockcount() // EPOCH_INTERVAL, epoch)
        self.walk_to_cutoff()
        self.wait_pool(epoch, intact)
        self.assert_no_refusals(offsets)
        # and, still within the epoch, it answers a challenge for it
        assert_equal(node.getblockcount() // EPOCH_INTERVAL, epoch)
        assert (epoch, int(revoked.proTxHash, 16)) not in asker.announcements
        asker.send_and_ping(msg_posechal(epoch))  # the answer comes before the pong
        assert (epoch, int(revoked.proTxHash, 16)) in asker.announcements

        self.log.info("Tests successful")


if __name__ == '__main__':
    DSLListChangeTest().main()
