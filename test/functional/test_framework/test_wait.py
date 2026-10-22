#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Deterministic polling and staged ChainLock recovery regressions; no nodes."""

from contextlib import contextmanager
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from feature_llmq_chainlocks import LLMQChainLocksTest
from test_framework import util
from test_framework.test_framework import DashTestFramework


@contextmanager
def fake_clock():
    clock = SimpleNamespace(now=0)

    def sleep(seconds):
        clock.now += seconds

    with patch.object(util.time, "time", side_effect=lambda: clock.now), patch.object(util.time, "sleep", side_effect=sleep):
        yield clock


def bare_framework():
    framework = DashTestFramework.__new__(DashTestFramework)
    framework.options = SimpleNamespace(timeout_factor=4)
    framework.bump_mocktime = Mock()
    return framework


def recovery_fixture(tip_at=1, is_at=2, cl_at=3, tx_count=4, missing_lock=None, cl_nodes=None):
    framework = LLMQChainLocksTest.__new__(LLMQChainLocksTest)
    framework.options = SimpleNamespace(timeout_factor=4)
    framework.mocktime = 1000
    framework.log = Mock()
    state = SimpleNamespace(tick=0, reads=[], locked=set())

    class Node:
        def __init__(self, index):
            self.index = index

        def getbestblockhash(self):
            state.reads.append(("tip", self.index, state.tick))
            return "target" if self.index == 0 or state.tick >= tip_at else "old"

        def getrawtransaction(self, txid, verbose):
            state.reads.append(("is", self.index, state.tick))
            assert state.tick >= tip_at, "IS was queried before the tips converged"
            locked = state.tick >= is_at and (self.index, txid) != missing_lock
            if locked:
                state.locked.add((self.index, txid))
            # A ChainLock-derived instantlock flag must not satisfy the IS stage.
            return {"instantlock": True, "instantlock_internal": locked}

        def getblock(self, block_hash):
            state.reads.append(("cl", self.index, state.tick))
            assert block_hash == "target"
            return {"confirmations": 1, "chainlock": state.tick >= cl_at and (cl_nodes is None or self.index in cl_nodes)}

    def bump_mocktime(step):
        assert step == 1
        framework.mocktime += step
        state.tick += step

    framework.bump_mocktime = bump_mocktime
    framework.nodes = [Node(i) for i in range(5)]
    return framework, state


class TestWaitCallbacks(unittest.TestCase):
    def test_default_polling_keeps_timeout_scaling_and_does_not_tick(self):
        framework = bare_framework()
        with fake_clock() as clock:
            self.assertFalse(framework.wait_until(lambda: False, timeout=1, sleep=1, do_assert=False))
        self.assertEqual(clock.now, 4)
        framework.bump_mocktime.assert_not_called()

    def test_callback_follows_failed_poll_outside_lock(self):
        lock = threading.Lock()
        events = []

        def predicate():
            self.assertTrue(lock.locked())
            events.append("poll")
            return events.count("poll") == 3

        def tick():
            self.assertFalse(lock.locked())
            events.append("tick")

        with fake_clock():
            self.assertTrue(util.wait_until_helper(predicate, lock=lock, wait_proc=tick))
        self.assertEqual(events, ["poll", "tick", "poll", "tick", "poll"])

    def test_immediate_success_does_not_call_callback(self):
        tick = Mock()
        with fake_clock() as clock:
            self.assertTrue(util.wait_until_helper(lambda: True, wait_proc=tick))
        self.assertEqual(clock.now, 0)
        tick.assert_not_called()

    def test_callback_error_is_not_swallowed_by_allow_exception(self):
        tick = Mock(side_effect=RuntimeError("tick failed"))
        with fake_clock(), self.assertRaisesRegex(RuntimeError, "tick failed"):
            util.wait_until_helper(lambda: False, wait_proc=tick, allow_exception=True)
        tick.assert_called_once()

    def test_chainlock_default_sleep_and_opt_in_forwarding(self):
        framework = bare_framework()
        framework.wait_until = Mock(return_value=True)
        framework.wait_for_chainlocked_block(Mock(), "target")
        self.assertEqual(framework.wait_until.call_args.kwargs["sleep"], 0.1)
        self.assertIsNone(framework.wait_until.call_args.kwargs["wait_proc"])
        tick = Mock()
        framework.wait_for_chainlocked_block(Mock(), "target", wait_proc=tick, sleep=1)
        self.assertEqual(framework.wait_until.call_args.kwargs["sleep"], 1)
        self.assertIs(framework.wait_until.call_args.kwargs["wait_proc"], tick)

    def test_negative_chainlock_expectation_still_rejects_success(self):
        framework = bare_framework()
        framework.wait_until = Mock(return_value=True)
        with self.assertRaisesRegex(AssertionError, "unexpectedly succeeded"):
            framework.wait_for_chainlocked_block(Mock(), "target", expected=False)

    def test_default_chainlock_wait_does_not_advance_mocktime(self):
        framework, state = recovery_fixture()
        node = Mock()
        node.getblock.return_value = {"confirmations": 1, "chainlock": False}
        with fake_clock(), self.assertLogs("TestFramework.utils", level="ERROR"), self.assertRaisesRegex(AssertionError, "not true after"):
            framework.wait_for_chainlocked_block(node, "target", timeout=1)
        self.assertEqual(state.tick, 0)
        self.assertEqual(framework.mocktime, 1000)

    def test_recovery_requires_tips_then_all_real_is_locks_then_cl(self):
        framework, state = recovery_fixture()
        with fake_clock():
            framework.wait_for_recovered_chainlock("target", ["a", "b", "c", "d"])
        self.assertEqual(state.tick, 3)
        self.assertEqual(len(state.locked), 16)
        self.assertEqual([tick for kind, index, tick in state.reads if kind == "cl" and index == 0], [2, 3])

    def test_recovery_budget_is_shared_and_cannot_age_into_a_chainlock(self):
        framework, state = recovery_fixture(tip_at=100, is_at=1000, cl_at=1000)
        with fake_clock(), self.assertRaisesRegex(AssertionError, "exhausted the IS-lock mocktime window"):
            framework.wait_for_recovered_chainlock("target", ["a", "b", "c", "d"])
        self.assertEqual(state.tick, 119)
        self.assertEqual(state.locked, set())
        self.assertFalse(any(kind == "cl" and index == 0 for kind, index, _ in state.reads))

    def test_recovery_checks_the_five_transactions_in_the_real_scenario(self):
        framework, state = recovery_fixture(tx_count=5)
        with fake_clock():
            framework.wait_for_recovered_chainlock("target", ["a", "b", "c", "d", "e"])
        self.assertEqual(len(state.locked), 20)
        self.assertEqual(state.tick, 3)
    def test_recovery_accepts_local_chainlock_when_one_mn_lacks_one_is_lock(self):
        framework, state = recovery_fixture(tx_count=5, missing_lock=(4, "e"))
        with fake_clock():
            framework.wait_for_recovered_chainlock("target", ["a", "b", "c", "d", "e"])
        self.assertNotIn((4, "e"), state.locked)
        self.assertEqual(len(state.locked), 19)
        self.assertEqual(state.tick, 3)
        self.assertTrue(any(kind == "cl" and index == 4 for kind, index, _ in state.reads))

    def test_recovery_does_not_substitute_another_nodes_chainlock(self):
        framework, state = recovery_fixture(is_at=1000, cl_at=0, cl_nodes={0})
        with fake_clock(), self.assertRaisesRegex(AssertionError, "exhausted the IS-lock mocktime window"):
            framework.wait_for_recovered_chainlock("target", ["a", "b", "c", "d"])
        self.assertEqual(state.tick, 119)
        self.assertEqual(state.locked, set())
