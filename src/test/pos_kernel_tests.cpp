// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/validation.h>
#include <pos/kernel.h>
#include <primitives/transaction.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pos_kernel_tests, TestChain100Setup)

namespace {
//! A transaction shaped like a coinstake (empty first output) spending `prevout`.
CMutableTransaction MakeCoinstakeSpending(const COutPoint& prevout)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(prevout);
    tx.vout.emplace_back(CTxOut());          // vout[0] empty: the coinstake marker
    tx.vout[0].SetEmpty();
    tx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    BOOST_REQUIRE(CTransaction(tx).IsCoinStake());
    return tx;
}
} // namespace

/**
 * Every rejection inside CheckProofOfStake must leave the state invalid.
 *
 * ConnectTip only calls InvalidBlockFound() when state.IsInvalid(); a rejection that
 * returns bare `false` therefore leaves the block unmarked, still a candidate in the
 * block index, and the next ActivateBestChain tries it again. Five of the eight
 * rejection paths used to do exactly that -- this test pins the contract for the two
 * that are reachable without a full staking setup, plus two that always set it.
 */
BOOST_AUTO_TEST_CASE(rejections_set_the_validation_state)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);
    BOOST_REQUIRE_EQUAL(tip->nHeight, 100);

    const unsigned int nBits = tip->nBits;
    uint256 hashProof, target;

    // A transaction that is not a coinstake at all: always set the state.
    {
        CMutableTransaction plain;
        plain.vin.emplace_back(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
        plain.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(plain), tip->nTime, nBits,
                                       hashProof, target));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "malformed-txn");
    }

    // An input that no coin backs: always set the state.
    {
        const auto tx = MakeCoinstakeSpending(COutPoint(uint256::ONE, 0));
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), tip->nTime, nBits,
                                       hashProof, target));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "prevout-not-found");
    }

    // Immature stake: the newest coinbase is 0 deep, well under COINBASE_MATURITY + 1.
    // This branch used to return a bare false and leave the block unmarked.
    {
        const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns.back()->GetHash(), 0));
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), tip->nTime, nBits,
                                       hashProof, target));
        BOOST_CHECK_MESSAGE(state.IsInvalid(),
                            "an immature stake must mark the block invalid, or ConnectTip retries it forever");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-depth");
    }

    // Mature coin, but the stake time equals the funding block time, so the input age is
    // 0 -- below stakeAgeRange[0]. Also a bare false before this change.
    {
        const CBlockIndex* funding = chainstate.m_chain[1];
        BOOST_REQUIRE(funding != nullptr);
        const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), funding->GetBlockTime(),
                                       nBits, hashProof, target));
        BOOST_CHECK_MESSAGE(state.IsInvalid(), "an out-of-range stake age must mark the block invalid");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-age");
    }
}

BOOST_AUTO_TEST_SUITE_END()
