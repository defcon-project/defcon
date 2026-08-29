// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/validation.h>
#include <pos/kernel.h>
#include <primitives/transaction.h>
#include <util/system.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <vector>

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

/**
 * A larger stake must never be less likely to win than a smaller one.
 *
 * The kernel hash does not depend on the amount -- only the stake modifier, the
 * outpoint and the two timestamps feed it -- so the amount moves the threshold
 * and nothing else. Weighting the target by multiplying it therefore has a
 * clean failure mode: the product exceeds 256 bits, arith_uint256 keeps the low
 * bits and drops the rest without a word, and the threshold lands somewhere
 * arbitrary. Two stakes that differ only in size then get thresholds in no
 * particular order.
 *
 * That happens exactly where the untruncated product would exceed every
 * possible hash, which is to say where the check should accept everything. At
 * the difficulty floor -- posLimit, which is where a chain sits when block
 * production has just started or has stalled -- it is reached by any stake of a
 * few dozen coins.
 */
BOOST_AUTO_TEST_CASE(a_larger_stake_is_never_ranked_below_a_smaller_one)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    Consensus::Params corrected = Params().GetConsensus();
    corrected.nPosKernelV2ActivationHeight = 0;
    Consensus::Params original = corrected;
    original.nPosKernelV2ActivationHeight = std::numeric_limits<int>::max();

    const unsigned int nBits = UintToArith256(corrected.posLimit).GetCompact();
    const COutPoint prevout(m_coinbase_txns[0]->GetHash(), 0);
    const CBlockIndex* funding = chainstate.m_chain[1];
    BOOST_REQUIRE(funding != nullptr);
    const uint32_t nBlockFromTime = static_cast<uint32_t>(funding->GetBlockTime());
    const uint32_t nTime = nBlockFromTime + 3600;

    const auto accepts = [&](const Consensus::Params& params, CAmount amount) {
        uint256 hashProof, target;
        return CheckStakeKernelHash(tip, params, nBits, nBlockFromTime, amount, prevout, nTime,
                                    hashProof, target);
    };

    std::vector<CAmount> amounts;
    for (int i = 1; i <= 400; ++i) {
        amounts.push_back(static_cast<CAmount>(i) * 5 * COIN);
    }

    // The corrected rules divide the hash by the weight, so a bigger weight can
    // only ever help: once a stake is large enough to be accepted, every larger
    // one must be too.
    bool accepted_something = false;
    for (const CAmount amount : amounts) {
        const bool ok = accepts(corrected, amount);
        if (accepted_something) {
            BOOST_CHECK_MESSAGE(ok, "a stake of " << amount / COIN
                                    << " was rejected while a smaller one was accepted");
        }
        accepted_something = accepted_something || ok;
    }
    BOOST_CHECK_MESSAGE(accepted_something, "no stake in the range was accepted at all");

    // The original rules do not hold that line. Find the inversion rather than
    // hard-coding one, so this states the property and not a fixture detail.
    CAmount smaller_accepted = 0, larger_rejected = 0;
    for (const CAmount amount : amounts) {
        if (accepts(original, amount)) {
            smaller_accepted = amount;
        } else if (smaller_accepted != 0) {
            larger_rejected = amount;
            break;
        }
    }
    BOOST_CHECK_MESSAGE(larger_rejected != 0,
                        "expected the original rules to rank some larger stake below a smaller one");
    BOOST_TEST_MESSAGE("original rules accepted a stake of " << smaller_accepted / COIN
                       << " and rejected the larger " << larger_rejected / COIN);
}

/**
 * Age alone must not retire a coin.
 *
 * A coin becomes ineligible once it is older than stakeAgeRange[1], and the
 * only thing that refreshes it is winning a block -- which is the one thing a
 * coin that never wins cannot do. Nothing in this kernel rewards age, so the
 * bound moderated no advantage; it simply removed coins from the set, silently
 * and for good.
 *
 * Under the corrected rules an over-age coin gets past the age check and is
 * judged on the same terms as any other. It still fails here, because this
 * coinstake carries no signature -- but it fails for that reason, which is the
 * point.
 */
BOOST_AUTO_TEST_CASE(an_old_coin_may_still_stake)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE_MESSAGE(IsPosKernelV2(params, tip->nHeight + 1),
                          "this test needs the corrected rules active on regtest");

    const CBlockIndex* funding = chainstate.m_chain[1];
    BOOST_REQUIRE(funding != nullptr);
    const int64_t past_the_cap = funding->GetBlockTime() + params.stakeAgeRange[1] + 1;

    const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
    BlockValidationState state;
    uint256 hashProof, target;
    BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), past_the_cap,
                                   tip->nBits, hashProof, target));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK_MESSAGE(state.GetRejectReason() != "bad-stake-age",
                        "an over-age coin was still turned away for its age");
}

/**
 * The same coin, under the rules the change replaced.
 *
 * an_old_coin_may_still_stake shows the corrected kernel letting an over-age
 * coin through. That only means something if the original kernel turned the
 * same coin away for the same reason, so this runs the identical fixture with
 * the activation height pushed out of reach. The age check sits ahead of the
 * signature check in CheckProofOfStake, which is why an unsigned coinstake is
 * enough to read the verdict: under these rules it never gets far enough to
 * fail for anything else.
 */
struct PosKernelOriginalRulesSetup : public TestChain100Setup {
    PosKernelOriginalRulesSetup() : TestChain100Setup{{"-testactivationheight=posv2@1000000"}} {}
};

BOOST_FIXTURE_TEST_CASE(an_old_coin_is_retired_under_the_original_rules, PosKernelOriginalRulesSetup)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE_MESSAGE(!IsPosKernelV2(params, tip->nHeight + 1),
                          "this test needs the original rules, so the override must have applied");

    const CBlockIndex* funding = chainstate.m_chain[1];
    BOOST_REQUIRE(funding != nullptr);
    const int64_t past_the_cap = funding->GetBlockTime() + params.stakeAgeRange[1] + 1;

    const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
    BlockValidationState state;
    uint256 hashProof, target;
    BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), past_the_cap,
                                   tip->nBits, hashProof, target));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-age");
}

/**
 * The activation height is the rollout, so it belongs in a test.
 *
 * Both halves matter and they fail in opposite directions. A devnet height
 * that moves invalidates every measurement taken across it -- the chain says
 * one rule applied where the record says another. A mainnet or testnet height
 * that appears by accident is a consensus change nobody decided to make, on a
 * network that is still running the original kernel deliberately.
 */
BOOST_AUTO_TEST_CASE(kernel_v2_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosKernelV2ActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nPosKernelV2ActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nPosKernelV2ActivationHeight,
                      0);

    // Devnet params assert on a devnet name being set; pow_tests does the same dance.
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nPosKernelV2ActivationHeight,
                      4000);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_SUITE_END()
