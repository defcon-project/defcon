// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chain.h>
#include <consensus/validation.h>
#include <evo/cbtx.h>
#include <evo/specialtx.h>
#include <llmq/chainlocks.h>
#include <llmq/context.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <util/check.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(evo_cbtx_tests, TestChain100Setup)

static CBlock BlockWithCbTx(const CCbTx& cbtx)
{
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_COINBASE;
    tx.vin.emplace_back();
    tx.vout.emplace_back(0, CScript() << OP_RETURN);
    SetTxPayload(tx, cbtx);

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(tx));
    return block;
}

// An attacker-supplied bestCLHeightDiff used to be subtracted from the block
// height without a range check; a diff at or above the height yields a
// negative chainlock height, GetAncestor() returns nullptr for it, and the
// unchecked dereference crashed the node on a crafted block. (dash#7363)
BOOST_AUTO_TEST_CASE(bestclheightdiff_out_of_range_is_rejected)
{
    const auto& clhandler = *Assert(m_node.llmq_ctx)->clhandler;

    // A fake index detached from any chain: pprev == nullptr means the
    // previous coinbase carries no chainlock, which is the path that reaches
    // the range check.
    CBlockIndex index;
    index.nHeight = 5;

    CBLSSecretKey sk;
    sk.MakeNewKey();

    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.nHeight = index.nHeight;
    cbtx.bestCLSignature = sk.Sign(uint256::ONE, false);
    BOOST_REQUIRE(cbtx.bestCLSignature.IsValid());

    // diff == nHeight points one block before genesis
    cbtx.bestCLHeightDiff = index.nHeight;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");
    }

    // The crafted-block extreme: bestCLHeightDiff travels as a CompactSize, so
    // MAX_SIZE is the largest value that deserializes at all
    cbtx.bestCLHeightDiff = MAX_SIZE;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");
    }
}

BOOST_AUTO_TEST_CASE(null_clsig_rules_unchanged)
{
    const auto& clhandler = *Assert(m_node.llmq_ctx)->clhandler;

    CBlockIndex index;
    index.nHeight = 5;

    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.nHeight = index.nHeight;

    // A null signature with a zero diff is the legitimate "no chainlock yet" form
    {
        BlockValidationState state;
        BOOST_CHECK(CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
    }

    // A null signature with a non-zero diff stays rejected
    cbtx.bestCLHeightDiff = 1;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");
    }
}

BOOST_AUTO_TEST_SUITE_END()
