// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/params.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(llmq_chainlocks_tests, BasicTestingSetup)

// The signed-height profile resolver is the single point that keeps signing
// and verification agreeing across the Q60 activation boundary. It must be
// one-way, exact at the boundary, and inert until both halves of the V2
// configuration are present.
BOOST_AUTO_TEST_CASE(chainlock_type_resolver)
{
    Consensus::Params params;
    params.llmqTypeChainLocks = Consensus::LLMQType::LLMQ_400_60;

    // No V2 configured: the legacy type at every height.
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 0) == Consensus::LLMQType::LLMQ_400_60);
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_400_60);

    // A V2 type without an activation height must never fire.
    params.llmqTypeChainLocksV2 = Consensus::LLMQType::LLMQ_DEFCON;
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_400_60);

    // The boundary is exact and one-way.
    params.nChainLocksV2ActivationHeight = 3240;
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 3239) == Consensus::LLMQType::LLMQ_400_60);
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 3240) == Consensus::LLMQType::LLMQ_DEFCON);
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_DEFCON);

    // An activation height without a V2 type is a half-written configuration
    // and must fail closed to the legacy profile.
    params.llmqTypeChainLocksV2 = Consensus::LLMQType::LLMQ_NONE;
    BOOST_CHECK(llmq::GetChainLocksLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_400_60);
}

// The Q60 profile's defining properties, pinned so a later edit cannot
// silently lose what the profile was selected for.
BOOST_AUTO_TEST_CASE(q60_profile_shape)
{
    const Consensus::LLMQParams* q60{nullptr};
    for (const auto& p : Consensus::available_llmqs) {
        if (p.type == Consensus::LLMQType::LLMQ_DEFCON) {
            q60 = &p;
        }
    }
    BOOST_REQUIRE(q60 != nullptr);

    BOOST_CHECK_EQUAL(q60->size, 60);
    BOOST_CHECK_EQUAL(q60->minSize, 44);
    BOOST_CHECK_EQUAL(q60->threshold, 41);

    // Two disjoint signer sets can never both reach the threshold: a
    // partition pauses finality instead of forking it.
    BOOST_CHECK(2 * q60->threshold > q60->size);
    BOOST_CHECK(q60->threshold <= q60->minSize);
    BOOST_CHECK(q60->minSize <= q60->size);

    // Marking a member bad takes a supermajority of the quorum, not the three
    // votes the inherited devnet profiles accepted.
    BOOST_CHECK_EQUAL(q60->dkgBadVotesThreshold, 48);
    BOOST_CHECK(q60->dkgBadVotesThreshold <= q60->size);

    // The mining window must start after the last DKG phase.
    BOOST_CHECK(q60->dkgMiningWindowStart >= 5 * q60->dkgPhaseBlocks);
    BOOST_CHECK(q60->dkgMiningWindowEnd > q60->dkgMiningWindowStart);
    BOOST_CHECK(q60->dkgMiningWindowEnd < q60->dkgInterval);
}

BOOST_AUTO_TEST_SUITE_END()
