#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""gobject list and gobject diff, the two RPCs one helper builds (F-2026-110).

gobject_list_helper(const bool make_a_diff) in src/rpc/governance.cpp builds
both RPCHelpMan objects and hands each its handler as a lambda. The lambda
captured the helper's make_a_diff parameter by reference and read it -- to
decide whether the last diff time applies -- on every call, long after the
helper had returned. Under AddressSanitizer that is a stack-use-after-return
report on the first `gobject list` or `gobject diff` and the daemon aborts;
without a sanitizer the read lands on whatever the stack holds at that moment.

This test drives exactly those two RPCs on one node with no governance objects,
through every branch the handler has: the defaults, each signal with each type,
the two refusals (answered as strings, not RPC errors), and a second diff after
the first has moved the diff time. It needs no masternode and no proposal,
because the capture is read before either matters. It is green on any build;
under --with-sanitizers=address it is the regression test, because the node
under test dies on the first call when the capture is a dangling reference.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

INVALID_SIGNAL = "Invalid signal, should be 'valid', 'funding', 'delete', 'endorsed' or 'all'"
INVALID_TYPE = "Invalid type, should be 'proposals', 'triggers' or 'all'"
SIGNALS = ("valid", "funding", "delete", "endorsed", "all")
TYPES = ("proposals", "triggers", "all")


class GobjectListDiffTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]

        self.log.info("gobject list and gobject diff with the default arguments")
        assert_equal(node.gobject("list"), {})
        assert_equal(node.gobject("diff"), {})

        self.log.info("every signal with every type, on both RPCs")
        for signal in SIGNALS:
            for objtype in TYPES:
                assert_equal(node.gobject("list", signal, objtype), {})
                assert_equal(node.gobject("diff", signal, objtype), {})

        self.log.info("the two refusals are answers, not errors")
        assert_equal(node.gobject("list", "bogus"), INVALID_SIGNAL)
        assert_equal(node.gobject("diff", "bogus"), INVALID_SIGNAL)
        assert_equal(node.gobject("list", "valid", "bogus"), INVALID_TYPE)
        assert_equal(node.gobject("diff", "valid", "bogus"), INVALID_TYPE)

        self.log.info("a second diff reads a moved last-diff time")
        assert_equal(node.gobject("diff"), {})
        assert_equal(node.gobject("diff"), {})


if __name__ == '__main__':
    GobjectListDiffTest().main()
