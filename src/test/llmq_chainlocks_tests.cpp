// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/params.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <test/util/setup_common.h>

#include <string>

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

    // The bound that survives review. A threshold at or below the Byzantine
    // minority would let that minority alone expel honest members and have
    // them PoSe-punished, since exclusion from validMembers is what the
    // punishment loop acts on.
    //
    // There is deliberately no upper bound here. An earlier reading derived one
    // from minSize -- at 44 seated only 43 can vote, so 48 cannot fire -- and
    // concluded that expulsion becomes impossible when most needed. It does
    // not: a member that sends no contribution is marked bad by every peer
    // independently at dkgsession.cpp:458, with no vote cast, and one
    // unanswered complaint is enough at :919. What this threshold governs is
    // narrower and worth keeping high: how many accusers condemn a member
    // before it is allowed to justify itself.
    BOOST_CHECK(q60->dkgBadVotesThreshold > q60->size / 3);

    // The mining window must start after the last DKG phase.
    BOOST_CHECK(q60->dkgMiningWindowStart >= 5 * q60->dkgPhaseBlocks);
    BOOST_CHECK(q60->dkgMiningWindowEnd > q60->dkgMiningWindowStart);
    BOOST_CHECK(q60->dkgMiningWindowEnd < q60->dkgInterval);
}

// The bad-votes resolver carries the same obligation as the ChainLock one:
// every member of a DKG session must resolve the same threshold from the
// quorum height alone. A member that resolves a different one computes a
// different valid-member set, and the session then agrees on no commitment at
// all -- which is why this is height-only and one-way rather than a flag.
BOOST_AUTO_TEST_CASE(dkg_bad_votes_threshold_resolver)
{
    Consensus::Params params;
    Consensus::LLMQParams profile{};
    profile.size = 400;
    profile.dkgBadVotesThreshold = 30;

    // A profile carrying no revision keeps its value at every height, even
    // once the network has an activation height.
    BOOST_CHECK_EQUAL(llmq::GetDkgBadVotesThreshold(params, profile, 0), 30);
    params.nDkgBadVotesV2ActivationHeight = 7416;
    BOOST_CHECK_EQUAL(llmq::GetDkgBadVotesThreshold(params, profile, 1000000), 30);

    // A revision on a network that never activates it must never fire.
    const Consensus::Params never{};
    profile.dkgBadVotesThresholdV2 = 300;
    BOOST_CHECK_EQUAL(llmq::GetDkgBadVotesThreshold(never, profile, 1000000), 30);

    // The boundary is exact, and it only ever moves forwards.
    BOOST_CHECK_EQUAL(llmq::GetDkgBadVotesThreshold(params, profile, 7415), 30);
    BOOST_CHECK_EQUAL(llmq::GetDkgBadVotesThreshold(params, profile, 7416), 300);
    BOOST_CHECK_EQUAL(llmq::GetDkgBadVotesThreshold(params, profile, 1000000), 300);
}

// What every profile does or does not revise, pinned.
//
// A zero is the deliberate "no revision", and it is also what a profile gets
// when a designated-initializer literal simply does not name the field -- so
// the danger is not a wrong number but a plausible one. Anything non-zero has
// to raise the threshold and has to stay reachable, because a bad-votes value
// that is too low is precisely what this revision exists to remove.
BOOST_AUTO_TEST_CASE(bad_votes_revisions_are_sane)
{
    const Consensus::LLMQParams* p400{nullptr};
    for (const auto& p : Consensus::available_llmqs) {
        if (p.dkgBadVotesThresholdV2 != 0) {
            BOOST_CHECK_MESSAGE(p.dkgBadVotesThresholdV2 > p.dkgBadVotesThreshold,
                                std::string(p.name) + " revises its bad-votes threshold downwards");
            BOOST_CHECK_MESSAGE(p.dkgBadVotesThresholdV2 <= p.size,
                                std::string(p.name) + " cannot reach its revised bad-votes threshold");
        }
        if (p.type == Consensus::LLMQType::LLMQ_400_60) {
            p400 = &p;
        }
    }
    BOOST_REQUIRE(p400 != nullptr);

    // 30 of 400 is 7.5% -- the same disproportion that made 3-of-50 the devnet
    // ban-wave engine. 300 is the upstream value, and the one llmq_400_85 in
    // this same table has carried all along.
    BOOST_CHECK_EQUAL(p400->dkgBadVotesThreshold, 30);
    BOOST_CHECK_EQUAL(p400->dkgBadVotesThresholdV2, 300);
}

BOOST_AUTO_TEST_SUITE_END()
