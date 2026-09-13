// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! The v23 activation bundle: one number, eight gates, two networks.
//!
//! Three things are pinned here. That mainnet and testnet are dormant today
//! with the Q60 profile already registered, so the activating commit is a
//! number and not code. That applying a height sets every gate in the bundle
//! and nothing outside it. And that a partial edit -- some heights set, some
//! not, or a height with no profile -- is refused at startup on the two
//! networks that release together, and only there.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <util/system.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(chainparams_v23_bundle_tests, BasicTestingSetup)

namespace {

constexpr int UNSET = std::numeric_limits<int>::max();
constexpr int Q60_DKG_INTERVAL = 24; // llmq_defcon, Consensus::available_llmqs

//! The eight heights the bundle owns, by name, so a failure names the field.
std::vector<std::pair<std::string, int>> BundleHeights(const Consensus::Params& c)
{
    return {
        {"nChainLocksV2ActivationHeight", c.nChainLocksV2ActivationHeight},
        {"nInstantSendV2ActivationHeight", c.nInstantSendV2ActivationHeight},
        {"nPosKernelV2ActivationHeight", c.nPosKernelV2ActivationHeight},
        {"nPosCoinbaseBoundActivationHeight", c.nPosCoinbaseBoundActivationHeight},
        {"nPosStakeModifierV2ActivationHeight", c.nPosStakeModifierV2ActivationHeight},
        {"nPosBlockTimeBoundActivationHeight", c.nPosBlockTimeBoundActivationHeight},
        {"nPosFeeBurnActivationHeight", c.nPosFeeBurnActivationHeight},
        {"nDkgBadVotesV2ActivationHeight", c.nDkgBadVotesV2ActivationHeight},
    };
}

void ExpectAllHeights(const Consensus::Params& c, int expected, const std::string& where)
{
    for (const auto& [name, height] : BundleHeights(c)) {
        BOOST_CHECK_MESSAGE(height == expected,
                            where + ": " + name + " is " + std::to_string(height) + ", expected " +
                                std::to_string(expected));
    }
}

} // namespace

// The state the tree ships in, and the state the DAO's commit starts from.
// A height appearing here by accident is a consensus change nobody decided to
// make; a profile missing here would make that commit add code, not a number.
BOOST_AUTO_TEST_CASE(mainnet_and_testnet_are_dormant_with_the_profile_registered)
{
    ArgsManager args;
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        const auto params = CreateChainParams(args, chain);
        const auto& c = params->GetConsensus();

        ExpectAllHeights(c, UNSET, chain);
        BOOST_CHECK_MESSAGE(c.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_NONE,
                            chain + " names a ChainLock switchover profile while the bundle is unset");
        BOOST_CHECK_MESSAGE(c.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_NONE,
                            chain + " names an InstantSend switchover profile while the bundle is unset");

        // Registered and dormant: the profile is present so that the activating
        // commit changes a number, and disabled because its height is unset.
        BOOST_CHECK_MESSAGE(params->GetLLMQ(Consensus::LLMQType::LLMQ_DEFCON).has_value(),
                            chain + " does not register llmq_defcon");

        // Outside the bundle by decision, and unset until a release says otherwise:
        // M-02 (dropped 2026-09-05), the Compute masternode type, and both DSL heights.
        BOOST_CHECK_EQUAL(c.nStrictBLSSigSizeActivationHeight, UNSET);
        BOOST_CHECK_EQUAL(c.nComputeNodeActivationHeight, UNSET);
        BOOST_CHECK_EQUAL(c.nDSLActivationHeight, UNSET);
        BOOST_CHECK_EQUAL(c.nDSLEnforcementHeight, UNSET);
    }
}

// One call, every gate, the same height -- and nothing outside the bundle.
BOOST_AUTO_TEST_CASE(apply_sets_the_eight_gates_and_both_profiles_together)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, CBaseChainParams::MAIN);
    constexpr int H = 7 * Q60_DKG_INTERVAL * 1000; // 168000, on the grid

    Consensus::Params scheduled = params->GetConsensus();
    ApplyV23ActivationBundle(scheduled, H);
    ExpectAllHeights(scheduled, H, "scheduled");
    BOOST_CHECK(scheduled.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_DEFCON);
    BOOST_CHECK(scheduled.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_DEFCON);
    // The dropped candidates are not the bundle's to touch.
    BOOST_CHECK_EQUAL(scheduled.nStrictBLSSigSizeActivationHeight, UNSET);
    BOOST_CHECK_EQUAL(scheduled.nComputeNodeActivationHeight, UNSET);
    BOOST_CHECK_EQUAL(scheduled.nDSLActivationHeight, UNSET);

    // Unset is a no-op, which is what keeps every network dormant today.
    Consensus::Params dormant = params->GetConsensus();
    ApplyV23ActivationBundle(dormant, UNSET);
    ExpectAllHeights(dormant, UNSET, "dormant");
    BOOST_CHECK(dormant.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_NONE);
    BOOST_CHECK(dormant.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_NONE);
}

// The formation lead is measured in DKG intervals from the height; a height
// off the grid silently shortens the first one. Refused, loudly, and before
// any field is written.
BOOST_AUTO_TEST_CASE(apply_refuses_a_height_off_the_grid_or_not_positive)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, CBaseChainParams::MAIN);

    for (const int bad : {1, Q60_DKG_INTERVAL + 1, 168001, 0, -Q60_DKG_INTERVAL}) {
        Consensus::Params c = params->GetConsensus();
        BOOST_CHECK_THROW(ApplyV23ActivationBundle(c, bad), std::runtime_error);
        // Nothing was written before the refusal.
        ExpectAllHeights(c, UNSET, "after refusing " + std::to_string(bad));
        BOOST_CHECK(c.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_NONE);
    }
}

// The startup guard: each of these is an edit someone can make while
// believing they have scheduled the release, and each is refused where a
// unit test sees it rather than where the chain does.
BOOST_AUTO_TEST_CASE(check_refuses_a_partial_edit_on_the_release_networks_only)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, CBaseChainParams::MAIN);
    constexpr int H = 168000;

    // Complete, and dormant: both fine.
    {
        Consensus::Params c = params->GetConsensus();
        BOOST_CHECK_NO_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN));
        ApplyV23ActivationBundle(c, H);
        BOOST_CHECK_NO_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN));
        BOOST_CHECK_NO_THROW(CheckV23ActivationBundle(c, CBaseChainParams::TESTNET));
    }
    // One height set on its own -- the edit this check exists for.
    {
        Consensus::Params c = params->GetConsensus();
        c.nChainLocksV2ActivationHeight = H;
        BOOST_CHECK_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN), std::runtime_error);
        BOOST_CHECK_THROW(CheckV23ActivationBundle(c, CBaseChainParams::TESTNET), std::runtime_error);
    }
    // Every height set, but one left behind.
    {
        Consensus::Params c = params->GetConsensus();
        ApplyV23ActivationBundle(c, H);
        c.nPosFeeBurnActivationHeight = UNSET;
        BOOST_CHECK_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN), std::runtime_error);
    }
    // Heights scheduled, profiles forgotten: the resolver would never switch.
    {
        Consensus::Params c = params->GetConsensus();
        ApplyV23ActivationBundle(c, H);
        c.llmqTypeDIP0024InstantSendV2 = Consensus::LLMQType::LLMQ_NONE;
        BOOST_CHECK_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN), std::runtime_error);
    }
    // Profiles named, heights unset: the mirror image.
    {
        Consensus::Params c = params->GetConsensus();
        c.llmqTypeChainLocksV2 = Consensus::LLMQType::LLMQ_DEFCON;
        c.llmqTypeDIP0024InstantSendV2 = Consensus::LLMQType::LLMQ_DEFCON;
        BOOST_CHECK_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN), std::runtime_error);
    }
    // M-02 is out of v23 by decision; a height for it here is that decision undone by accident.
    {
        Consensus::Params c = params->GetConsensus();
        c.nStrictBLSSigSizeActivationHeight = H;
        BOOST_CHECK_THROW(CheckV23ActivationBundle(c, CBaseChainParams::MAIN), std::runtime_error);
    }
    // The same partial edit is legitimate where gates are scheduled one by one.
    {
        Consensus::Params c = params->GetConsensus();
        c.nChainLocksV2ActivationHeight = H;
        c.nStrictBLSSigSizeActivationHeight = 0;
        BOOST_CHECK_NO_THROW(CheckV23ActivationBundle(c, CBaseChainParams::REGTEST));
        BOOST_CHECK_NO_THROW(CheckV23ActivationBundle(c, CBaseChainParams::DEVNET));
    }
}

// Regtest can schedule the bundle the way the release does, so a functional
// test can cross H with every rule flipping at once -- and it can still
// schedule the gates one by one, at different heights, which is what tells a
// resolver reading its own height from one reading another's.
BOOST_AUTO_TEST_CASE(regtest_v23_alias_schedules_the_whole_bundle)
{
    {
        ArgsManager args;
        args.ForceSetArg("-testactivationheight", "v23@480");
        const auto params = CreateChainParams(args, CBaseChainParams::REGTEST);
        const auto& c = params->GetConsensus();
        ExpectAllHeights(c, 480, "regtest v23@480");
        BOOST_CHECK(c.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_DEFCON);
        BOOST_CHECK(c.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_DEFCON);
        BOOST_CHECK(params->GetLLMQ(Consensus::LLMQType::LLMQ_DEFCON).has_value());
    }
    {
        ArgsManager args;
        args.ForceSetArg("-testactivationheight", "v23@481");
        BOOST_CHECK_THROW(CreateChainParams(args, CBaseChainParams::REGTEST), std::runtime_error);
    }
    {
        // The per-gate names keep working, at heights of their own.
        ArgsManager args;
        args.ForceSetArg("-testactivationheight", "chainlocksv2@600");
        const auto params = CreateChainParams(args, CBaseChainParams::REGTEST);
        const auto& c = params->GetConsensus();
        BOOST_CHECK_EQUAL(c.nChainLocksV2ActivationHeight, 600);
        BOOST_CHECK_EQUAL(c.nInstantSendV2ActivationHeight, UNSET);
        BOOST_CHECK_EQUAL(c.nPosKernelV2ActivationHeight, 0); // regtest's own default, untouched
        BOOST_CHECK(c.llmqTypeChainLocksV2 == Consensus::LLMQType::LLMQ_DEFCON);
        BOOST_CHECK(c.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_NONE);
    }
}

// Testnet is where the DAO rehearses the switchover, so its quorum table has to
// be mainnet's: the same registered profiles in the same order and the same
// profile in every role, before and after H. A difference means the rehearsal
// crosses from a profile mainnet does not use, or punishes on DKG rounds mainnet
// never runs.
BOOST_AUTO_TEST_CASE(testnet_quorum_table_is_mainnets)
{
    ArgsManager args;
    const auto main = CreateChainParams(args, CBaseChainParams::MAIN);
    const auto test = CreateChainParams(args, CBaseChainParams::TESTNET);
    const auto& m = main->GetConsensus();
    const auto& t = test->GetConsensus();

    std::vector<Consensus::LLMQType> main_types, test_types;
    for (const auto& p : m.llmqs) main_types.push_back(p.type);
    for (const auto& p : t.llmqs) test_types.push_back(p.type);
    BOOST_CHECK(main_types == test_types);
    BOOST_CHECK_EQUAL(test_types.size(), 6U);
    BOOST_CHECK(!test->GetLLMQ(Consensus::LLMQType::LLMQ_25_67).has_value());

    BOOST_CHECK(t.llmqTypeChainLocks == m.llmqTypeChainLocks);
    BOOST_CHECK(t.llmqTypeChainLocks == Consensus::LLMQType::LLMQ_400_60);
    BOOST_CHECK(t.llmqTypeDIP0024InstantSend == m.llmqTypeDIP0024InstantSend);
    BOOST_CHECK(t.llmqTypePlatform == m.llmqTypePlatform);
    BOOST_CHECK(t.llmqTypeMnhf == m.llmqTypeMnhf);
    BOOST_CHECK(t.llmqTypeChainLocksV2 == m.llmqTypeChainLocksV2);
    BOOST_CHECK(t.llmqTypeDIP0024InstantSendV2 == m.llmqTypeDIP0024InstantSendV2);
}

namespace {

//! The two profiles mainnet registers and never forms. The rule reads the
//! selected network, so each case selects it for real rather than faking it.
void CheckRegisteredButUnformedProfiles(bool expect_formed)
{
    CBlockIndex index;
    index.nHeight = 100000;
    for (const auto type : {Consensus::LLMQType::LLMQ_50_60, Consensus::LLMQType::LLMQ_60_75}) {
        BOOST_CHECK_EQUAL(llmq::IsQuorumTypeEnabledInternal(type, &index, false, false), expect_formed);
    }
}

struct TestNetSetup : public BasicTestingSetup {
    TestNetSetup() : BasicTestingSetup(CBaseChainParams::TESTNET) {}
};

struct DevNetSetup : public BasicTestingSetup {
    DevNetSetup() : BasicTestingSetup(CBaseChainParams::DEVNET, {"-devnet=v23-bundle-tests", "-listen=0"}) {}
};

} // namespace

BOOST_AUTO_TEST_CASE(mainnet_never_forms_its_unused_profiles)
{
    BOOST_REQUIRE_EQUAL(Params().NetworkIDString(), CBaseChainParams::MAIN);
    CheckRegisteredButUnformedProfiles(/*expect_formed=*/false);
}

BOOST_FIXTURE_TEST_CASE(testnet_never_forms_mainnets_unused_profiles, TestNetSetup)
{
    BOOST_REQUIRE_EQUAL(Params().NetworkIDString(), CBaseChainParams::TESTNET);
    CheckRegisteredButUnformedProfiles(/*expect_formed=*/false);
}

// The control: devnet still forms them (stopping that there is a separate,
// height-gated change), so the two checks above can fail and are not reading a
// constant.
BOOST_FIXTURE_TEST_CASE(devnet_still_forms_them, DevNetSetup)
{
    BOOST_REQUIRE_EQUAL(Params().NetworkIDString(), CBaseChainParams::DEVNET);
    CheckRegisteredButUnformedProfiles(/*expect_formed=*/true);
}

BOOST_AUTO_TEST_SUITE_END()
