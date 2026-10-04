#!/usr/bin/env python3
"""Regression coverage for startup readiness and failure-path shutdown."""

import http.client
import io
import logging
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNode


class Clock:
    def __init__(self):
        self.now = 0.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Process:
    def __init__(self):
        self.returncode = None
        self.terminated = False

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = 0

    def kill(self):
        raise AssertionError("graceful cleanup must not need kill")


class LifecycleTest(unittest.TestCase):
    def node(self, *, timeout=120, factor=2):
        node = TestNode(
            0, "/unused", [], chain="regtest", rpchost=None,
            timewait=timeout, timeout_factor=factor, bitcoind="/unused/defcond",
            bitcoin_cli="/unused/defcon-cli", mocktime=0, coverage_dir=None,
            cwd=None, extra_args=[],
        )
        node.cleanup_on_exit = False
        node.running = True
        node.process = Process()
        node.stdout = io.BytesIO()
        node.stderr = io.BytesIO()
        self.addCleanup(node.stdout.close)
        self.addCleanup(node.stderr.close)
        return node

    def wait_for_readiness(self, node, clock, loaded_after):
        rpc = Mock()
        rpc.getblockcount.return_value = 1
        rpc.getmempoolinfo.side_effect = lambda: {"loaded": clock.now >= loaded_after}
        with patch("test_framework.test_node.get_rpc_proxy", return_value=rpc), \
                patch("test_framework.test_node.rpc_url", return_value="http://unused"), \
                patch("test_framework.util.time", clock):
            node.wait_for_rpc_connection()
        return rpc

    def test_readiness_after_sixty_seconds(self):
        node, clock = self.node(), Clock()
        rpc = self.wait_for_readiness(node, clock, loaded_after=65)
        self.assertEqual(clock.now, 65)
        self.assertTrue(node.rpc_connected)
        self.assertIs(node.rpc, rpc)

    def test_readiness_deadline_is_not_scaled_twice(self):
        node, clock = self.node(), Clock()
        with self.assertLogs("TestFramework.utils", level="ERROR"), \
                self.assertRaisesRegex(AssertionError, "after 120.*seconds"):
            self.wait_for_readiness(node, clock, loaded_after=121)
        self.assertEqual(clock.now, 120)
        self.assertFalse(node.rpc_connected)

    def test_stop_before_rpc_is_connected(self):
        node = self.node()
        process = node.process
        node.stop_node()
        self.assertTrue(process.terminated)
        self.assertFalse(node.running)
        self.assertIsNone(node.process)

    def test_broken_rpc_uses_graceful_signal(self):
        for error in [http.client.CannotSendRequest(), ConnectionResetError()]:
            with self.subTest(error=type(error).__name__):
                node = self.node()
                process = node.process
                node.rpc_connected = True
                node.rpc = Mock()
                node.rpc.stop.side_effect = error
                with self.assertLogs(node.log, level="ERROR"), \
                        patch("test_framework.util.time", Clock()):
                    node.stop_node()
                self.assertTrue(process.terminated)
                self.assertFalse(node.running)

    def test_connected_rpc_keeps_normal_stop(self):
        node = self.node()
        process = node.process
        node.rpc_connected = True
        rpc = node.rpc = Mock()
        rpc.stop.side_effect = lambda **kwargs: setattr(process, "returncode", 0)
        node.stop_node(wait=3)
        rpc.stop.assert_called_once_with(wait=3)
        self.assertFalse(process.terminated)
        self.assertFalse(node.running)

    def test_cli_can_stop_without_an_rpc_proxy(self):
        node = self.node()
        process = node.process
        node.use_cli = True
        node.stop = Mock(side_effect=lambda **kwargs: setattr(process, "returncode", 0))
        node.stop_node()
        node.stop.assert_called_once_with(wait=0)
        self.assertFalse(process.terminated)

    def test_stderr_error_is_preserved(self):
        node = self.node()
        node.stderr.write(b"unexpected stderr")
        with self.assertRaisesRegex(AssertionError, "Unexpected stderr"):
            node.stop_node()
        self.assertTrue(node.process.terminated)

    def test_nonzero_exit_is_preserved(self):
        node = self.node()
        node.process.terminate = lambda: setattr(node.process, "returncode", 1)
        with self.assertRaisesRegex(AssertionError, "non-zero exit code"):
            node.stop_node()

    def stop_all(self, first_stop_error=None, first_wait_error=None):
        nodes = [Mock(index=i) for i in range(3)]
        if first_stop_error:
            nodes[0].stop_node.side_effect = first_stop_error
        if first_wait_error:
            nodes[0].wait_until_stopped.side_effect = first_wait_error
        framework = SimpleNamespace(nodes=nodes, log=logging.getLogger("LifecycleCleanup"))
        with self.assertLogs(framework.log, level="ERROR"), self.assertRaises(Exception) as error:
            BitcoinTestFramework.stop_nodes(framework)
        for node in nodes:
            node.stop_node.assert_called_once_with(expected_stderr="", wait=0, wait_until_stopped=False)
            node.wait_until_stopped.assert_called_once_with()
        self.assertIs(error.exception, first_stop_error or first_wait_error)

    def test_one_stop_failure_does_not_skip_other_nodes(self):
        self.stop_all(first_stop_error=AssertionError("stderr failed"))

    def test_one_wait_failure_does_not_skip_other_nodes(self):
        self.stop_all(first_wait_error=AssertionError("shutdown timed out"))

    def test_first_failure_survives_later_cleanup_failure(self):
        self.stop_all(
            first_stop_error=AssertionError("original shutdown failure"),
            first_wait_error=AssertionError("later timeout"),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
