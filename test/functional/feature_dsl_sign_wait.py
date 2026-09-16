#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sign complete Sentinel verdicts early, but do not wait beyond the last block.

Use real report-delay faults and quorum signatures. Unlike the relay scenarios,
this test keeps the default signing policy enabled on every node. A delayed
report set must yield its full verdict; reports still absent at the deadline
must yield an actual signed, mined unobserved verdict, not a missing commitment.
Members that sign the same verdict on different ticks, more than a signing
session timeout apart, must still reach a commitment: a member that has signed
asks again on the later ticks of the window until the signature is recovered.
"""

import re
import struct

from feature_dsl_scenarios import ARGS, CUTOFF, EPOCH, SIGNING, DSLScenariosTest, canonical
from test_framework.messages import hash256, ser_string
from test_framework.util import assert_equal, assert_greater_than_or_equal, force_finish_mnsync


class DSLSignWaitTest(DSLScenariosTest):
    def set_test_params(self):
        self.set_dash_test_params(8, 7, extra_args=[ARGS] * 8)

    SESSION_TIMEOUT = 60  # CSigSharesManager::SESSION_NEW_SHARES_TIMEOUT, seconds

    def signed_lines(self, member, epoch):
        return re.findall(r"DSL -- asked quorum .* to sign epoch %d \(format v2\), missed=\d+, unobserved=\d+" % epoch,
                          member.debug_log_path.read_text(encoding="utf-8"))

    def resigned_lines(self, member, epoch):
        return re.findall(r"DSL -- asked quorum .* to sign epoch %d again at position \d+" % epoch,
                          member.debug_log_path.read_text(encoding="utf-8"))

    def timed_out_lines(self, member, request_id):
        return re.findall(r"signing session timed out\. signHash=[0-9a-f]+, id=%s," % request_id,
                          member.debug_log_path.read_text(encoding="utf-8"))

    def share_lines(self, member, request_id):
        """The sign hashes of the shares this member made for the request."""
        return re.findall(r"created sigShare\. signHash=([0-9a-f]+), id=%s," % request_id,
                          member.debug_log_path.read_text(encoding="utf-8"))

    def request_id(self, node, epoch):
        base = bytes.fromhex(node.getblockhash(epoch * EPOCH))[::-1]
        return hash256(ser_string(b"dslcommitment") + struct.pack("<I", epoch) + base)[::-1].hex()

    def has_signature(self, node, epoch):
        # isconflicting is true only for a recovered signature with a different
        # message hash. Two distinct probes cover any possible recovered hash;
        # unlike a sleep, this proves the producer actually has the signature.
        request_id = self.request_id(node, epoch)
        return (node.quorum("isconflicting", 100, request_id, "00" * 32) or
                node.quorum("isconflicting", 100, request_id, "11" * 32))

    def wait_for_signature(self, node, epoch):
        self.wait_until(lambda: self.has_signature(node, epoch), timeout=60)

    def staggered_epoch(self):
        """The three members sign the same complete verdict on three different
        ticks (+21, +22, +23), each more than a signing-session timeout after the
        previous one. A session forgets its shares 60 s after the last new one,
        and a member never signs the same request twice on its own, so without
        re-signing no two shares ever coexist and the epoch closes without a
        commitment although every member signed the same bytes."""
        node = self.nodes[0]
        assert_equal(node.getblockcount() % EPOCH, 0)
        base = node.getblockcount()
        epoch = base // EPOCH
        # commitment-skip holds while height < expiry: the second member first
        # signs at +22 and the third at +23; the first is not held at all.
        faults = [member.faultinject("set", "commitment-skip", base + SIGNING + i, "sign-stagger")
                  for i, member in enumerate(self.signers) if i > 0]
        assert_equal([f["expiryHeight"] for f in faults], [base + SIGNING + 1, base + SIGNING + 2])
        self.bump_mocktime(60)
        self.wait_until(lambda: node.dslstatus()["epoch"] == epoch, timeout=30)
        self.wait_until(lambda: all(n.dslstatus()["respondedcount"] == len(self.mninfo) for n in self.nodes), timeout=60)
        self.bump_mocktime(30)
        self.generate(node, CUTOFF)
        self.wait_until(lambda: all(n.dslstatus()["epochreports"] > 0 for n in self.nodes), timeout=90)
        self.settle(node)
        request_id = self.request_id(node, epoch)
        self.bump_mocktime(10)
        self.generate(node, SIGNING - CUTOFF)  # +21: only the first member signs
        for i, member in enumerate(self.signers):
            if i > 0:
                previous = self.signers[i - 1]
                # The share is made on a worker thread after the request; the
                # clock must not jump before it exists, or its session would
                # simply start after the jump and never time out. A member whose
                # request found the signature already recovered makes no share.
                self.wait_until(lambda: self.share_lines(previous, request_id) or self.has_signature(node, epoch),
                                timeout=30)
                # More than a session timeout after the previous signer's tick,
                # then the next tick: the previous signer's own session is gone
                # by then, unless the signature was recovered first, in which
                # case the session was closed as done and nothing times out.
                seen = len(self.timed_out_lines(previous, request_id))
                self.bump_mocktime(self.SESSION_TIMEOUT + 5)
                self.wait_until(lambda: len(self.timed_out_lines(previous, request_id)) > seen or
                                        self.has_signature(node, epoch), timeout=30)
                self.generate(node, 1)
            self.wait_until(lambda: len(self.signed_lines(member, epoch)) == 1, timeout=30)
            assert_equal(int(self.signed_lines(member, epoch)[0].rsplit("unobserved=", 1)[1]), 0)
        assert_equal(node.getblockcount() % EPOCH, EPOCH - 1)
        self.wait_for_signature(node, epoch)
        # The lone +21 share timed out at least once on the member that made it.
        # Real-time recovery after a resign is not bounded by the window: under
        # load the +22 resign's share can itself go unmet before +23, timing out
        # again and resigning a second time -- still within the window, and still
        # a commitment. Exactly-one would be a claim about scheduling speed, not
        # about the mechanism this test exists to prove.
        assert_greater_than_or_equal(len(self.timed_out_lines(self.signers[0], request_id)), 1)
        c = self.close(node)
        assert c is not None, "members that sign on different ticks must still produce a commitment"
        assert_equal(c["epoch"], epoch)
        assert_equal(c["missedCount"], 0)
        assert_equal(c["unobservedIndices"], [])
        assert_equal(node.faultinject("list")["faults"], [])  # expired by height
        for member in self.signers:
            assert_equal(len(self.signed_lines(member, epoch)), 1)
        # the mechanism: the +21 signer asked again on a later tick, for the same
        # hash, and that is when its share finally met another member's
        assert_greater_than_or_equal(len(self.resigned_lines(self.signers[0], epoch)), 1)

    def delayed_epoch(self, delay, deadline):
        node = self.nodes[0]
        target = self.mninfo[0].proTxHash
        target_node = self.mn_node(target)
        late = [self.mn_node(m.proTxHash) for m in self.mninfo[1:3]]
        scenario = "sign-wait-deadline" if deadline else "sign-wait-late"
        faults = [(target_node, target_node.faultinject("set", "response-drop", self.far(node), scenario))]
        faults += [(n, n.faultinject("set", "report-delay", self.far(node), scenario, delay)) for n in late]
        self.close(node)
        epoch = node.getblockcount() // EPOCH
        self.wait_until(lambda: all(n.dslstatus()["respondedcount"] == 6 for n in self.nodes), timeout=60)
        self.generate(node, CUTOFF)
        self.wait_until(lambda: all(target in n.dslstatus()["candidate"]["unobservedprotxhashes"]
                                   and n.dslstatus()["epochreports"] > 0 for n in self.nodes), timeout=60)
        # Wait for the undelayed reports to settle before the signing offset.
        self.settle(node)
        self.generate(node, SIGNING - CUTOFF)
        for member in self.signers:
            expected = "DSL -- holding commitment signing for epoch %d at position %d" % (epoch, SIGNING)
            self.wait_until(lambda: expected in member.debug_log_path.read_text(encoding="utf-8"), timeout=30)
            assert_equal(self.signed_lines(member, epoch), [])

        self.generate(node, 1)  # +22: the two late sentinels may now emit
        if not deadline:
            self.wait_until(lambda: all(n.dslstatus()["candidate"]["unobservedcount"] == 0
                                       and n.dslstatus()["candidate"]["missedprotxhashes"] == [target]
                                       for n in self.nodes), timeout=60)
        else:
            for member in self.signers:
                expected = "DSL -- holding commitment signing for epoch %d at position %d" % (epoch, EPOCH - 2)
                self.wait_until(lambda: expected in member.debug_log_path.read_text(encoding="utf-8"), timeout=30)
                assert_equal(self.signed_lines(member, epoch), [])

        self.generate(node, 1)  # +23: sign even if reports are still absent
        for member in self.signers:
            self.wait_until(lambda: len(self.signed_lines(member, epoch)) == 1, timeout=30)
            line = self.signed_lines(member, epoch)[0]
            unobserved = int(line.rsplit("unobserved=", 1)[1])
            if deadline:
                assert unobserved > 0, line
            else:
                assert_equal(unobserved, 0)
        self.wait_for_signature(node, epoch)
        # No further tick runs in this epoch. Clear before opening the next one
        # so its liveness announcement is not suppressed by the old fault.
        for n, fault in faults:
            assert_equal(n.faultinject("clear", fault["id"])["cleared"], 1)
        c = self.close(node)
        assert c is not None, "the deadline must produce a commitment, not skip the epoch"
        assert_equal(c["epoch"], epoch)
        order = canonical([m["proRegTxHash"] for m in node.protx("diff", 1, epoch * EPOCH)["mnList"]])
        if deadline:
            assert order.index(target) in c["unobservedIndices"]
            assert_equal(c["missedCount"], 0)
        else:
            assert_equal(c["unobservedIndices"], [])
            assert_equal(self.missed_protx(node, c), [target])
        for member in self.signers:
            assert_equal(len(self.signed_lines(member, epoch)), 1)

    def run_test(self):
        node = self.nodes[0]
        self.wait_for_sporks_same()
        for n in self.nodes:
            force_finish_mnsync(n)
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        quorum_hash = self.mine_quorum()
        self.signers = [self.mn_node(m["proTxHash"]) for m in node.quorum("info", 100, quorum_hash)["members"]]
        assert_equal(len(self.signers), 3)
        self.align(node)
        self.close(node)
        self.walk(node, len(self.mninfo))  # warm-up
        self.close(node)
        self.walk(node, len(self.mninfo))  # full, undelayed control
        self.log.info("Reports arriving before the deadline produce the full MISSED verdict")
        self.delayed_epoch(delay=4, deadline=False)
        self.walk(node, len(self.mninfo))  # recovery, ends just before the next boundary
        self.log.info("Reports absent at the deadline produce a signed unobserved verdict")
        self.delayed_epoch(delay=6, deadline=True)
        self.log.info("Members signing on different ticks, a session timeout apart, still commit")
        self.staggered_epoch()


if __name__ == '__main__':
    DSLSignWaitTest().main()
