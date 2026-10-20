#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test EOF handling when a short P2P message and FIN are already queued."""

import os
import signal
import socket
import threading

from test_framework.messages import msg_sendheaders
from test_framework.p2p import NetworkThread, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class EpollEOFTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-socketevents=epoll"]]

    def skip_test_if_missing_module(self):
        self.skip_if_platform_not_linux()

    def run_test(self):
        node = self.nodes[0]
        for backend in ["epoll", "poll", "select"]:
            if backend != "epoll":
                self.restart_node(0, extra_args=[f"-socketevents={backend}"])
            self.log.info("Test queued message and FIN with %s", backend)
            peer = node.add_p2p_connection(P2PInterface())
            assert_equal(len(node.getpeerinfo()), 1)
            peer_id = node.getpeerinfo()[0]["id"]
            payload = peer.build_message(msg_sendheaders())
            sock = peer._transport.get_extra_info("socket")
            sent = threading.Event()
            errors = []

            def send_and_half_close():
                try:
                    peer._transport.write(payload)
                    peer._transport.write_eof()
                except Exception as error:
                    errors.append(error)
                finally:
                    sent.set()

            # Stop only this disposable child so its next recv() sees both the
            # short message and EOF. The kernel continues to acknowledge TCP.
            pid = node.process.pid
            with node.assert_debug_log([f"socket closed for peer={peer_id}"]):
                os.kill(pid, signal.SIGSTOP)
                try:
                    self.wait_until(lambda: os.waitpid(pid, os.WUNTRACED | os.WNOHANG)[0] == pid)
                    NetworkThread.network_event_loop.call_soon_threadsafe(send_and_half_close)
                    assert sent.wait(5 * self.options.timeout_factor), "P2P thread did not send the message and FIN"
                    assert not errors, errors
                    # Linux TCP_FIN_WAIT2 (5) means the remote kernel has ACKed
                    # our FIN; the data and EOF are queued before we resume.
                    self.wait_until(lambda: sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 1)[0] == 5)
                finally:
                    os.kill(pid, signal.SIGCONT)
                self.wait_until(lambda: not node.getpeerinfo(), timeout=5)
            node.disconnect_p2ps()


if __name__ == '__main__':
    EpollEOFTest().main()
