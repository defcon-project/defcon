// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <evo/evodb.h>
#include <llmq/chainlocks.h>
#include <llmq/context.h>
#include <primitives/block.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

/**
 * Regression coverage for F-2026-119.
 *
 * ConnectBlock's proof-of-stake branch marked pindex dirty before knowing
 * whether the block was being connected at all. Under fJustCheck the caller
 * owns pindex, and TestBlockValidity's lives in its own stack frame, so the
 * pointer left behind in m_dirty_blockindex outlives that frame; the next
 * FlushStateToDisk copies the set into WriteBatchSync and serialises whatever
 * the dead frame now holds into the block tree database.
 *
 * What this case can and cannot assert. m_dirty_blockindex is private to
 * BlockManager, whose only friends are CChainState and ChainstateManager, so a
 * test cannot read it. Two things are still provable here, and together they
 * are the regression:
 *
 *   1. That execution reaches the offending line. The case asserts the exact
 *      rejection reason "prevout-not-found", which the whole tree raises in one
 *      place only -- CheckProofOfStake, in pos/kernel.cpp -- and which
 *      ConnectBlock can only reach after the insert. So that reason, and no
 *      other, proves the insert ran.
 *   2. That the flush which would dereference the pointer is actually reached.
 *
 * The dereference itself is undefined behaviour, so whether it is observable
 * was measured rather than assumed: on the unfixed code this case segfaults,
 * five runs out of five, on an ordinary -O2 build. No sanitizer is needed for
 * it to bite. The fix's evidence package carries those runs.
 */
BOOST_FIXTURE_TEST_SUITE(pos_dirty_index_tests, TestChain100Setup)

namespace {
//! Puts the fixture's chain into the proof-of-stake regime for a scope, so a
//! block built on the tip is judged by the proof-of-stake rules. Nothing is
//! connected while the override is in force; it only changes what ConnectBlock
//! reads. Restores on scope exit.
struct ScopedPosRegime {
    ScopedPosRegime(Consensus::Params& params, int last_pow_block) :
        m_params(params), m_saved_last_pow(params.lastPowBlock)
    {
        m_params.lastPowBlock = last_pow_block;
    }
    ~ScopedPosRegime() { m_params.lastPowBlock = m_saved_last_pow; }
    Consensus::Params& m_params;
    const int m_saved_last_pow;
};
} // namespace

BOOST_AUTO_TEST_CASE(testing_a_proof_of_stake_block_leaves_no_dirty_index_entry)
{
    auto& chainman = *Assert(m_node.chainman);
    auto& chainstate = chainman.ActiveChainstate();

    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip())};
    BOOST_REQUIRE(tip != nullptr);
    const int tip_height{tip->nHeight};

    // A block on top of the tip, reshaped into a proof-of-stake block by
    // inserting a coinstake at vtx[1]. The coinstake need not be spendable:
    // the insert under test happens before the proof-of-stake check that will
    // refuse it.
    CBlock block = CreateBlock({}, CScript() << OP_TRUE, chainstate);

    // A proof-of-stake block's coinbase carries the masternode payout and its
    // first output is empty; CheckBlock refuses anything else.
    CMutableTransaction coinbase{*block.vtx[0]};
    coinbase.vout.resize(1);
    coinbase.vout[0].SetEmpty();
    block.vtx[0] = MakeTransactionRef(std::move(coinbase));

    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vin[0].prevout = COutPoint(uint256::ONE, 0);
    coinstake.vout.resize(2);
    coinstake.vout[0].SetEmpty();
    coinstake.vout[1].nValue = 1 * COIN;
    coinstake.vout[1].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.insert(block.vtx.begin() + 1, MakeTransactionRef(std::move(coinstake)));
    BOOST_REQUIRE(block.IsProofOfStake());

    // A proof-of-stake block carries a zero nonce; the rule is not gated.
    block.nNonce = 0;

    // ...and a time on the stake grid, strictly after its predecessor's. The
    // minter picks its search time the same way, by masking the clock down.
    const uint32_t mask{static_cast<uint32_t>(Params().GetConsensus().posTimestampMask)};
    block.nTime = (static_cast<uint32_t>(tip->GetBlockTime()) + mask + 1) & ~mask;
    BOOST_REQUIRE(static_cast<int64_t>(block.nTime) > tip->GetBlockTime());

    // Judge the block by the proof-of-stake rules: put the boundary below it.
    ScopedPosRegime regime{const_cast<Consensus::Params&>(Params().GetConsensus()), tip_height - 1};

    BlockValidationState state;
    {
        LOCK(cs_main);
        // The work target has to be recomputed under the override: past the
        // boundary GetNextWorkRequired answers by a different rule than the one
        // CreateBlock used, and the header check compares against the new one.
        block.nBits = GetNextWorkRequired(chainstate.m_chain.Tip(), &block, Params().GetConsensus());
        // fCheckMerkleRoot is off, which also turns off the block-signature
        // check: this case is about index bookkeeping, not about signing.
        BOOST_CHECK(!TestBlockValidity(state, *Assert(m_node.llmq_ctx)->clhandler,
                                       *Assert(m_node.evodb), Params(), chainstate, block,
                                       chainstate.m_chain.Tip(),
                                       /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
    }

    // The proof that the offending line was executed at all. "prevout-not-found"
    // is raised in exactly one place in the tree, CheckProofOfStake
    // (pos/kernel.cpp), and ConnectBlock calls that only after the dirty-index
    // insert -- so this reason, and no other, means the insert was reached and
    // the coinstake was then refused for its unspendable kernel. If this ever
    // starts failing, the case has stopped covering the code it exists for and
    // must be repaired rather than deleted.
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "prevout-not-found");

    // With an entry left behind, this walks a pointer into a dead stack frame.
    // It has to be reached for the sanitizer control to mean anything.
    WITH_LOCK(cs_main, chainstate.ForceFlushStateToDisk());

    // Nothing was connected: the tip is where it was.
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Height()), tip_height);
}

BOOST_AUTO_TEST_SUITE_END()
