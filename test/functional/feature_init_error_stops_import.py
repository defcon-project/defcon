#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A start that fails after the block import has begun asks the import to stop.

defcond used to go from a failed start straight to its shutdown, which waits
for the block import thread, without telling that thread to stop: a node whose
start failed during a long -reindex or -loadblock stayed up until the whole
import had run.

The import is held at a known point: -loadblock names a FIFO, and opening it
blocks the import thread until the test opens the other end. The start fails
meanwhile on a -bind address that does not resolve, which is read only after
the import thread has been started. Once the shutdown is under way the test
releases the FIFO with nothing in it; the import thread then reaches its first
shutdown check, and has to leave there.
"""

import os
import tempfile

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

BIND_ERROR = "Error: Cannot resolve -bind address: 'unresolvable.invalid'"
# PrepareShutdown's first line: defcond calls it after its handling of the failed start.
SHUTDOWN_UNDER_WAY = "PrepareShutdown: In progress..."
# Written by the import thread once it has opened the FIFO.
IMPORT_OPENED = "Importing blocks file"
IMPORT_STOPPED = "Shutdown requested. Exit ThreadImport"


class InitErrorStopsImportTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        # os.mkfifo
        self.skip_if_platform_not_linux()

    def log_since(self, offset):
        with open(self.nodes[0].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(offset)
            return f.read()

    def run_test(self):
        node = self.nodes[0]
        # With a chain on disk the start does not wait for the import thread to
        # connect the genesis block; it goes on while that thread is held.
        self.generate(node, 1, sync_fun=self.no_op)
        self.stop_node(0)

        fifo = os.path.join(self.options.tmpdir, "blocks.fifo")
        os.mkfifo(fifo)
        offset = node.debug_log_path.stat().st_size
        with tempfile.NamedTemporaryFile(dir=node.stderr_dir, delete=False) as stderr:
            node.start(extra_args=[f"-loadblock={fifo}", "-bind=unresolvable.invalid"], stderr=stderr)

            self.log.info("The start fails while the import thread waits on the FIFO")
            self.wait_until(lambda: SHUTDOWN_UNDER_WAY in self.log_since(offset) or node.process.poll() is not None)
            assert node.process.poll() is None, "the node exited while the import thread was held"
            log = self.log_since(offset)
            assert BIND_ERROR in log, log
            assert IMPORT_OPENED not in log, log

            self.log.info("Released, the import thread finds the shutdown request and leaves")
            writer = None

            def import_thread_waits():
                nonlocal writer
                try:
                    writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                except OSError:
                    # ENXIO: the import thread has not opened its end yet
                    return False
                return True

            self.wait_until(import_thread_waits)
            os.close(writer)
            ret = node.process.wait(timeout=self.rpc_timeout)
            node.running = False
            node.process = None
            assert ret != 0, "the failed start exited with success"
            stderr.seek(0)
            assert BIND_ERROR in stderr.read().decode("utf-8")

        log = self.log_since(offset)
        assert IMPORT_OPENED in log, log
        assert IMPORT_STOPPED in log, "the import thread was not asked to stop"

        self.log.info("The node then starts as usual")
        os.remove(fifo)
        self.start_node(0)
        assert_equal(node.getblockcount(), 1)


if __name__ == '__main__':
    InitErrorStopsImportTest().main()
