// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Retiring a quorum profile from a height (Consensus::Params::llmqFormationEndHeights).
//!
//! Pinned here: which networks retire what, and where; that the formation gate
//! turns exactly at the height and touches no other profile; and that the startup
//! check refuses every configuration that would stop a profile somebody still
//! depends on or stop it mid-cycle. The chain-level behaviour across the height
//! -- rounds before it form, none after, a reorg back across it -- is
//! feature_llmq_formation_end.py.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <util/system.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

BOOST_FIXTURE_TEST_SUITE(llmq_formation_end_tests, BasicTestingSetup)

namespace {

constexpr int UNSET = std::numeric_limits<int>::max();

//! The devnet is built from its name, which it reads from the global args.
struct DevNetArg {
    DevNetArg() { gArgs.SoftSetBoolArg("-devnet", true); }
    ~DevNetArg() { gArgs.ForceRemoveArg("devnet"); }
};

struct DevNetSetup : public BasicTestingSetup {
    DevNetSetup() : BasicTestingSetup(CBaseChainParams::DEVNET, {"-devnet=formation-end-tests", "-listen=0"}) {}
};

struct RegTestRetiringSetup : public BasicTestingSetup {
    RegTestRetiringSetup() : BasicTestingSetup(CBaseChainParams::REGTEST, {"-llmqformationendheight=llmq_test_instantsend:480"}) {}
};

bool FormsNextBlock(Consensus::LLMQType type, int prev_height)
{
    CBlockIndex index;
    index.nHeight = prev_height;
    return llmq::IsQuorumTypeEnabledInternal(type, &index, /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false);
}

} // namespace

// Mainnet, testnet and regtest retire nothing; devnet retires exactly the two
// profiles mainnet never forms, at one height on both their grids.
BOOST_AUTO_TEST_CASE(networks_retire_what_they_should)
{
    ArgsManager args;
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(args, chain);
        BOOST_CHECK_MESSAGE(params->GetConsensus().llmqFormationEndHeights.empty(), chain + " retires a quorum profile");
    }

    DevNetArg devnet_arg;
    const auto devnet = CreateChainParams(args, CBaseChainParams::DEVNET);
    const auto& ends = devnet->GetConsensus().llmqFormationEndHeights;
    BOOST_REQUIRE_EQUAL(ends.size(), 2U);
    const int end_50_60 = llmq::GetQuorumFormationEndHeight(devnet->GetConsensus(), Consensus::LLMQType::LLMQ_50_60);
    const int end_60_75 = llmq::GetQuorumFormationEndHeight(devnet->GetConsensus(), Consensus::LLMQType::LLMQ_60_75);
    BOOST_CHECK(end_50_60 != UNSET);
    BOOST_CHECK_EQUAL(end_50_60, end_60_75);
    BOOST_CHECK_EQUAL(end_50_60 % 48, 0);
    // Everything that signs on devnet keeps forming.
    for (const auto type : {Consensus::LLMQType::LLMQ_DEFCON, Consensus::LLMQType::LLMQ_400_60,
                            Consensus::LLMQType::LLMQ_400_85, Consensus::LLMQType::LLMQ_100_67}) {
        BOOST_CHECK_EQUAL(llmq::GetQuorumFormationEndHeight(devnet->GetConsensus(), type), UNSET);
    }
}

// The gate reads "the block after pindexPrev": a cycle based one block below
// the end still starts, the one based at the end does not.
BOOST_FIXTURE_TEST_CASE(devnet_gate_turns_exactly_at_the_height, DevNetSetup)
{
    BOOST_REQUIRE_EQUAL(Params().NetworkIDString(), CBaseChainParams::DEVNET);
    const auto& consensus = Params().GetConsensus();
    const int end = llmq::GetQuorumFormationEndHeight(consensus, Consensus::LLMQType::LLMQ_50_60);
    BOOST_REQUIRE(end != UNSET);

    for (const auto type : {Consensus::LLMQType::LLMQ_50_60, Consensus::LLMQType::LLMQ_60_75}) {
        BOOST_CHECK(FormsNextBlock(type, end - 2)); // next block end-1
        BOOST_CHECK(!FormsNextBlock(type, end - 1)); // next block is the end
        BOOST_CHECK(!FormsNextBlock(type, end));
        BOOST_CHECK(!FormsNextBlock(type, end + 1000));
    }
    // A profile nobody retired is untouched on both sides.
    BOOST_CHECK_EQUAL(FormsNextBlock(Consensus::LLMQType::LLMQ_400_60, end - 2), FormsNextBlock(Consensus::LLMQType::LLMQ_400_60, end));
    BOOST_CHECK(FormsNextBlock(Consensus::LLMQType::LLMQ_400_60, end));
}

// The regtest argument is what the functional test uses; it must land in the
// same field and turn the same gate.
BOOST_FIXTURE_TEST_CASE(regtest_argument_sets_the_same_gate, RegTestRetiringSetup)
{
    const auto& consensus = Params().GetConsensus();
    BOOST_CHECK_EQUAL(llmq::GetQuorumFormationEndHeight(consensus, Consensus::LLMQType::LLMQ_TEST_INSTANTSEND), 480);
    BOOST_CHECK_EQUAL(llmq::GetQuorumFormationEndHeight(consensus, Consensus::LLMQType::LLMQ_TEST), UNSET);
    BOOST_CHECK(FormsNextBlock(Consensus::LLMQType::LLMQ_TEST_INSTANTSEND, 478));
    BOOST_CHECK(!FormsNextBlock(Consensus::LLMQType::LLMQ_TEST_INSTANTSEND, 479));
    BOOST_CHECK(FormsNextBlock(Consensus::LLMQType::LLMQ_TEST, 479));
}

BOOST_AUTO_TEST_CASE(regtest_argument_refuses_malformed_input)
{
    for (const std::string& bad : {"llmq_test_instantsend", "llmq_test_instantsend:", "llmq_test_instantsend:-24",
                                    "llmq_test_instantsend:0", "llmq_test_instantsend:x", "llmq_nonexistent:480"}) {
        ArgsManager args;
        args.ForceSetArg("-llmqformationendheight", bad);
        BOOST_CHECK_THROW(CreateChainParams(args, CBaseChainParams::REGTEST), std::runtime_error);
    }
}

// Each configuration below stops a profile somebody still depends on, or stops
// it mid-cycle. Regtest is the canvas: its roles are LLMQ_TEST (ChainLocks,
// MNHF), LLMQ_TEST_DIP0024 (InstantSend, rotating) and LLMQ_TEST_PLATFORM.
BOOST_AUTO_TEST_CASE(startup_check_refuses_what_would_break)
{
    ArgsManager args;
    const auto with_end = [&](Consensus::LLMQType type, int end) {
        auto params = CreateChainParams(args, CBaseChainParams::REGTEST);
        const_cast<Consensus::Params&>(params->GetConsensus()).llmqFormationEndHeights[type] = end;
        return params;
    };

    // The one the functional test uses is accepted.
    BOOST_CHECK_NO_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_TEST_INSTANTSEND, 480)));

    // Off the grid: the last cycle below the end would still be mining past it.
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_TEST_INSTANTSEND, 481)), std::runtime_error);
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_TEST_INSTANTSEND, 0)), std::runtime_error);
    // Not registered here.
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_400_60, 480)), std::runtime_error);
    // Live roles: ChainLocks with no switchover scheduled, MNHF, Platform.
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_TEST, 480)), std::runtime_error);
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_TEST_PLATFORM, 480)), std::runtime_error);
    // A rotating profile.
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*with_end(Consensus::LLMQType::LLMQ_TEST_DIP0024, 480)), std::runtime_error);

    // A pre-switchover role stops only after its successor has taken over, and
    // not at the switchover itself. On devnet llmq_60_75 signed InstantSend
    // below 7200 and llmq_400_60 signed ChainLocks below 3240.
    DevNetArg devnet_arg;
    auto devnet = CreateChainParams(args, CBaseChainParams::DEVNET);
    BOOST_CHECK_NO_THROW(CheckLLMQConfiguration(*devnet));
    auto& dc = const_cast<Consensus::Params&>(devnet->GetConsensus());
    BOOST_REQUIRE(dc.llmqTypeDIP0024InstantSend == Consensus::LLMQType::LLMQ_60_75);
    dc.llmqFormationEndHeights[Consensus::LLMQType::LLMQ_60_75] = dc.nInstantSendV2ActivationHeight - 48;
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*devnet), std::runtime_error);
    dc.llmqFormationEndHeights[Consensus::LLMQType::LLMQ_60_75] = dc.nInstantSendV2ActivationHeight;
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*devnet), std::runtime_error);
    dc.llmqFormationEndHeights[Consensus::LLMQType::LLMQ_60_75] = dc.nInstantSendV2ActivationHeight + 48;
    BOOST_CHECK_NO_THROW(CheckLLMQConfiguration(*devnet));

    BOOST_REQUIRE(dc.llmqTypeChainLocks == Consensus::LLMQType::LLMQ_400_60);
    dc.llmqFormationEndHeights[Consensus::LLMQType::LLMQ_400_60] = dc.nChainLocksV2ActivationHeight;
    BOOST_CHECK_THROW(CheckLLMQConfiguration(*devnet), std::runtime_error);
    dc.llmqFormationEndHeights[Consensus::LLMQType::LLMQ_400_60] = dc.nChainLocksV2ActivationHeight + 72;
    BOOST_CHECK_NO_THROW(CheckLLMQConfiguration(*devnet));
}

// What the refusal above is for, asked of the running code rather than of the
// check: whenever the check accepts an end for a pre-switchover role, that
// role's resolver never names the profile at a tip where the gate has already
// disabled it. A tip like that is where IsQuorumActive, scanning at the tip,
// refuses the sig shares and recovered sigs of the profile's own quorums, so a
// lock that is still due cannot be signed. Every end on the grid from two
// intervals below the switchover to two above is tried, each against the tips
// around both heights.
BOOST_FIXTURE_TEST_CASE(accepted_end_keeps_a_pre_switchover_role_live, DevNetSetup)
{
    auto& c = const_cast<Consensus::Params&>(Params().GetConsensus());
    const auto check_role = [&](const std::string& role, Consensus::LLMQType type, int switchover, bool instantsend) {
        const auto profile = Params().GetLLMQ(type);
        BOOST_REQUIRE(profile.has_value());
        const auto resolve = [&](int height) {
            return instantsend ? llmq::GetInstantSendLLMQType(c, height) : llmq::GetChainLocksLLMQType(c, height);
        };
        const auto saved = c.llmqFormationEndHeights;
        const int interval = profile->dkgInterval;
        const int first_grid = (switchover / interval) * interval;
        int accepted = 0;
        for (int end = first_grid - 2 * interval; end <= first_grid + 2 * interval; end += interval) {
            c.llmqFormationEndHeights = saved;
            c.llmqFormationEndHeights[type] = end;
            bool ok{true};
            try {
                CheckLLMQConfiguration(Params());
            } catch (const std::runtime_error&) {
                ok = false;
            }
            if (!ok) continue;
            ++accepted;
            for (int tip = std::min(end, switchover) - 3; tip <= std::max(end, switchover) + 1; ++tip) {
                if (resolve(tip) != type) continue;
                BOOST_CHECK_MESSAGE(FormsNextBlock(type, tip),
                                    role << ": the check accepted end " << end << ", but at tip " << tip
                                         << " the resolver still names " << profile->name << " and the gate has disabled it");
            }
        }
        // Not vacuous: some end near the switchover is accepted.
        BOOST_CHECK_MESSAGE(accepted > 0, role << ": no end near the switchover was accepted");
        c.llmqFormationEndHeights = saved;
    };

    check_role("InstantSend", c.llmqTypeDIP0024InstantSend, c.nInstantSendV2ActivationHeight, /*instantsend=*/true);
    check_role("ChainLocks", c.llmqTypeChainLocks, c.nChainLocksV2ActivationHeight, /*instantsend=*/false);
}

BOOST_AUTO_TEST_SUITE_END()
