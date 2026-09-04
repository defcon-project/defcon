// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <util/system.h>

#include <limits>
#include <memory>
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

// Formation of the switchover profile is keyed on the activation height alone.
// It used to be keyed on the network name as well, which made the height
// unusable anywhere but devnet: a network could name the switchover, register
// the profile and set the height, and still never form a single quorum -- so at
// the height the resolver moved onto a profile with nothing to sign, and
// ChainLocks stopped with nothing in the configuration to explain it.
BOOST_AUTO_TEST_CASE(q60_formation_lead)
{
    const Consensus::LLMQParams* q60{nullptr};
    for (const auto& p : Consensus::available_llmqs) {
        if (p.type == Consensus::LLMQType::LLMQ_DEFCON) {
            q60 = &p;
        }
    }
    BOOST_REQUIRE(q60 != nullptr);

    // Quorums must exist by the time the resolver flips, so formation opens a
    // whole signing set of DKG cycles earlier -- and not one block before that,
    // because a commitment of a type old binaries do not know forks them off.
    // That quiet stretch is the window a fleet has to finish upgrading in.
    const int lead = (q60->signingActiveQuorumCount + 1) * q60->dkgInterval;
    BOOST_CHECK_EQUAL(lead, 120);
    BOOST_CHECK(lead > q60->signingActiveQuorumCount * q60->dkgInterval);
}

// A network that has not scheduled the switchover leaves the height
// unreachable, and nothing may key off that value arithmetically: the guard has
// to read it as "never" before it is used, or the lead subtraction alone would
// open formation on every network at once.
BOOST_AUTO_TEST_CASE(unset_switchover_height_never_forms)
{
    ArgsManager args;
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET,
                                     CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(args, chain);
        const auto& consensus = params->GetConsensus();
        BOOST_CHECK_MESSAGE(consensus.nChainLocksV2ActivationHeight == std::numeric_limits<int>::max(),
                            chain + " has scheduled the Q60 switchover; this test needs updating "
                                    "and so does every deployment plan that assumed it had not");
        BOOST_CHECK_MESSAGE(consensus.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_NONE,
                            chain + " names a Q60 switchover profile without a height");
    }
}

// The startup coherence check. Each of these configurations is one somebody can
// write while believing they have deployed the switchover, and each fails in a
// way the chain, not the log, would report.
BOOST_AUTO_TEST_CASE(llmq_configuration_coherence)
{
    ArgsManager args;
    static constexpr int UNSET = std::numeric_limits<int>::max();

    // A devnet is named, and reads its name from the global args while it is
    // being built -- the same dance pos_kernel_tests does, held by a guard so
    // a failing check below cannot leave the flag set for the next suite.
    struct DevNetArg {
        DevNetArg() { gArgs.SoftSetBoolArg("-devnet", true); }
        ~DevNetArg() { gArgs.ForceRemoveArg("devnet"); }
    } devnet_arg;

    // Everything this binary ships is coherent as configured.
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET,
                                     CBaseChainParams::DEVNET, CBaseChainParams::REGTEST}) {
        BOOST_CHECK_NO_THROW(CreateChainParams(args, chain));
    }

    // A switchover onto a profile the network does not register. GetLLMQ then
    // answers nullopt and its consumers assert on it, so the first CLSIG at or
    // above the height aborts the process -- and a peer supplies that CLSIG.
    {
        auto params = CreateChainParams(args, CBaseChainParams::MAIN);
        auto& consensus = const_cast<Consensus::Params&>(params->GetConsensus());
        BOOST_REQUIRE(!params->GetLLMQ(Consensus::LLMQType::LLMQ_DEFCON).has_value());
        consensus.llmqTypeChainLocksV2 = Consensus::LLMQType::LLMQ_DEFCON;
        consensus.nChainLocksV2ActivationHeight = 1000;
        BOOST_CHECK_THROW(CheckLLMQConfiguration(*params), std::runtime_error);
    }

    const auto devnet = [&]() { return CreateChainParams(args, CBaseChainParams::DEVNET); };
    BOOST_REQUIRE(devnet()->GetLLMQ(Consensus::LLMQType::LLMQ_DEFCON).has_value());

    // A profile with no height never activates.
    {
        auto params = devnet();
        auto& consensus = const_cast<Consensus::Params&>(params->GetConsensus());
        consensus.nChainLocksV2ActivationHeight = UNSET;
        consensus.llmqTypeDIP0024InstantSendV2 = Consensus::LLMQType::LLMQ_NONE;
        consensus.nInstantSendV2ActivationHeight = UNSET;
        BOOST_CHECK_THROW(CheckLLMQConfiguration(*params), std::runtime_error);
    }

    // A height with no profile quietly keeps the legacy one.
    {
        auto params = devnet();
        auto& consensus = const_cast<Consensus::Params&>(params->GetConsensus());
        consensus.llmqTypeChainLocksV2 = Consensus::LLMQType::LLMQ_NONE;
        BOOST_CHECK_THROW(CheckLLMQConfiguration(*params), std::runtime_error);
    }

    // InstantSend cannot move onto the switchover profile before the switchover
    // itself: below that height the quorums it would sign on do not exist yet.
    {
        auto params = devnet();
        auto& consensus = const_cast<Consensus::Params&>(params->GetConsensus());
        consensus.nInstantSendV2ActivationHeight = consensus.nChainLocksV2ActivationHeight - 1;
        BOOST_CHECK_THROW(CheckLLMQConfiguration(*params), std::runtime_error);
    }

    // Equal heights are allowed: the quorums have been forming since the lead.
    {
        auto params = devnet();
        auto& consensus = const_cast<Consensus::Params&>(params->GetConsensus());
        consensus.nInstantSendV2ActivationHeight = consensus.nChainLocksV2ActivationHeight;
        BOOST_CHECK_NO_THROW(CheckLLMQConfiguration(*params));
    }
}

// The decision itself, on a network that is not devnet.
//
// This is the property the old guard denied: it returned false for every
// network but devnet before it ever read the height, so no mainnet
// configuration could open formation. Regtest stands in for "some network that
// scheduled the switchover" -- what matters is only that it is not devnet, so
// the pre-fix code would answer false at every height below.
BOOST_FIXTURE_TEST_CASE(formation_follows_the_height_not_the_network, RegTestingSetup)
{
    using namespace llmq;
    BOOST_REQUIRE(Params().NetworkIDString() != CBaseChainParams::DEVNET);

    const CBlockIndex* tip = m_node.chainman->ActiveTip();
    BOOST_REQUIRE(tip != nullptr);

    Consensus::Params& consensus = const_cast<Consensus::Params&>(Params().GetConsensus());

    // Put everything back however this case ends: these params are global and
    // outlive the test.
    struct Restore {
        Consensus::Params& c;
        const std::vector<Consensus::LLMQParams> llmqs;
        const Consensus::LLMQType type;
        const int height;
        explicit Restore(Consensus::Params& params) :
            c(params), llmqs(params.llmqs), type(params.llmqTypeChainLocksV2),
            height(params.nChainLocksV2ActivationHeight) {}
        ~Restore() {
            c.llmqs = llmqs;
            c.llmqTypeChainLocksV2 = type;
            c.nChainLocksV2ActivationHeight = height;
        }
    } restore{consensus};

    // Register the profile the way AddLLMQ would, and schedule the switchover.
    BOOST_REQUIRE(!Params().GetLLMQ(Consensus::LLMQType::LLMQ_DEFCON).has_value());
    for (const auto& profile : Consensus::available_llmqs) {
        if (profile.type == Consensus::LLMQType::LLMQ_DEFCON) {
            consensus.llmqs.push_back(profile);
        }
    }
    const auto q60 = Params().GetLLMQ(Consensus::LLMQType::LLMQ_DEFCON);
    BOOST_REQUIRE(q60.has_value());
    consensus.llmqTypeChainLocksV2 = Consensus::LLMQType::LLMQ_DEFCON;

    const int lead = (q60->signingActiveQuorumCount + 1) * q60->dkgInterval;
    const int next = tip->nHeight + 1;

    // Scheduled, but the lead has not been reached: still closed.
    consensus.nChainLocksV2ActivationHeight = next + lead + 1;
    BOOST_CHECK(!IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_DEFCON, tip, false, false));

    // Exactly at the lead: open. Under the network-name guard this was false,
    // and would have stayed false all the way past the activation height.
    consensus.nChainLocksV2ActivationHeight = next + lead;
    BOOST_CHECK(IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_DEFCON, tip, false, false));

    // Past the switchover it stays open -- the profile has to keep forming the
    // quorums that sign after it.
    consensus.nChainLocksV2ActivationHeight = next - 1;
    BOOST_CHECK(IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_DEFCON, tip, false, false));

    // And an unscheduled network never forms it, however it is registered.
    consensus.nChainLocksV2ActivationHeight = std::numeric_limits<int>::max();
    BOOST_CHECK(!IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_DEFCON, tip, false, false));
}

BOOST_AUTO_TEST_SUITE_END()
