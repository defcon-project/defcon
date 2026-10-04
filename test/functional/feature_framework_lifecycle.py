#!/usr/bin/env python3
"""Exercise delayed readiness and RPC-less shutdown with real daemons."""

import time
import os
from unittest.mock import patch

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework import test_node
from test_framework.util import assert_equal


class DelayedReadiness:
    def __init__(self, rpc, delay):
        self.rpc = rpc
        self.ready_at = time.monotonic() + delay

    def getmempoolinfo(self):
        result = self.rpc.getmempoolinfo()
        result["loaded"] = result["loaded"] and time.monotonic() >= self.ready_at
        return result

    def __getattr__(self, name):
        return getattr(self.rpc, name)


class FrameworkLifecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.supports_cli = False
        self.rpc_timeout = 120

    def skip_test_if_missing_module(self):
        if os.name == "nt":
            raise SkipTest("RPC-less graceful shutdown requires POSIX SIGTERM")

    def run_test(self):
        make_proxy = test_node.get_rpc_proxy

        def delayed_proxy(*args, **kwargs):
            return DelayedReadiness(make_proxy(*args, **kwargs), 65)

        started = time.monotonic()
        with patch.object(test_node, "get_rpc_proxy", delayed_proxy):
            self.restart_node(0)
        elapsed = time.monotonic() - started
        assert elapsed >= 65, elapsed
        assert self.nodes[0].rpc_connected
        self.log.info("Delayed readiness succeeded after %.1f seconds", elapsed)

        # Reproduce the state left by a failed startup: a live daemon without
        # an installed RPC proxy. The second daemon retains its normal proxy.
        self.nodes[0].rpc_connected = False
        self.nodes[0].rpc = None
        self.stop_nodes()
        for node in self.nodes:
            assert_equal(node.running, False)
            assert_equal(node.process, None)
            assert "Shutdown: done" in node.debug_log_path.read_text()
        self.log.info("Both daemons stopped cleanly; RPC-less node used SIGTERM")


if __name__ == "__main__":
    FrameworkLifecycleTest().main()
