// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! The Dash rule packages DeFCoN's public networks never activate: V19, V20 and MN_RR. Mainnet, testnet and devnet
//! leave all three heights unset, and selecting one of those networks refuses
//! any of them set, whatever its value. Regtest keeps them, switched on for the tests inherited from Dash.
//! (The BRR, DIP0020 and DIP0024 deployments are gone; llmq_100_67, which the DIP0020 deployment enabled, never
//! forms, and no quorum profile rotates.)
//!
//! The check runs in SelectParams, on the network chosen, and not where every network's parameters are built: the
//! help text builds mainnet's, testnet's and regtest's before the command line has chosen, and a refusal there would
//! stop every network. That a refused height actually stops the node, and only on that network, is measured on the
//! binary with a source variant (no command line reaches these heights outside regtest).

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <llmq/options.h>
#include <tinyformat.h>
#include <util/system.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int UNSET = std::numeric_limits<int>::max();

using Field = std::function<int&(Consensus::Params&)>;

//! The three heights, by name, so a failure names the field.
std::vector<std::pair<std::string, Field>> DashRuleHeights()
{
    return {
        {"V19Height", [](Consensus::Params& c) -> int& { return c.V19Height; }},
        {"V20Height", [](Consensus::Params& c) -> int& { return c.V20Height; }},
        {"MN_RRHeight", [](Consensus::Params& c) -> int& { return c.MN_RRHeight; }},
    };
}

//! A network as it starts: all three heights unset, and the check passes.
void ExpectNoneSet(const Consensus::Params& consensus, const std::string& network)
{
    for (const auto& [name, field] : DashRuleHeights()) {
        Consensus::Params copy{consensus};
        BOOST_CHECK_MESSAGE(field(copy) == UNSET, strprintf("%s: %s is %d", network, name, field(copy)));
    }
    BOOST_CHECK_NO_THROW(CheckNeverActivatedDashRules(consensus, network));
}

//! Each height set on its own, at the first block, at an ordinary height or just below unset, is refused by name.
void ExpectEachRefused(const Consensus::Params& consensus, const std::string& network)
{
    for (const auto& [name, field] : DashRuleHeights()) {
        for (const int height : {0, 1, 1000, UNSET - 1}) {
            Consensus::Params copy{consensus};
            field(copy) = height;
            BOOST_CHECK_EXCEPTION(CheckNeverActivatedDashRules(copy, network), std::runtime_error,
                                  HasReason(strprintf("%s is %d on %s", name, height, network)));
        }
    }
}

//! Selects `chain` (SelectParams: the check included) and checks what it selected; the fixture's network is selected
//! again afterwards, as the other tests expect it.
void ExpectSelected(const std::string& chain, const std::string& restore)
{
    BOOST_CHECK_NO_THROW(SelectParams(chain));
    BOOST_CHECK_EQUAL(Params().NetworkIDString(), chain);
    if (chain != CBaseChainParams::REGTEST) ExpectNoneSet(Params().GetConsensus(), chain);
    SelectParams(restore);
}

//! llmq_100_67, Dash's platform profile, which mainnet, testnet and devnet register: the DIP0020 deployment enabled
//! it, and with that deployment gone it forms at no height (as before: on these three networks the deployment's
//! height was unreachable). Reads the selected network.
void ExpectPlatformProfileNeverForms(const std::string& network)
{
    BOOST_REQUIRE_EQUAL(Params().NetworkIDString(), network);
    BOOST_REQUIRE(Params().GetLLMQ(Consensus::LLMQType::LLMQ_100_67).has_value());
    for (const int prev_height : {0, 1, 100000, UNSET - 2}) {
        CBlockIndex index;
        index.nHeight = prev_height;
        BOOST_CHECK_MESSAGE(!llmq::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_100_67, &index),
                            strprintf("%s: llmq_100_67 forms after height %d", network, prev_height));
    }
}

struct DashRulesDevNetSetup : public BasicTestingSetup {
    DashRulesDevNetSetup() : BasicTestingSetup(CBaseChainParams::DEVNET, {"-devnet=dash-rules-tests", "-listen=0"}) {}
};

struct DashRulesRegTestSetup : public BasicTestingSetup {
    DashRulesRegTestSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(chainparams_dash_rules_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_and_testnet_leave_them_unset_and_refuse_each)
{
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        ArgsManager args;
        const auto params = CreateChainParams(args, chain);
        BOOST_REQUIRE_EQUAL(params->NetworkIDString(), chain);
        ExpectNoneSet(params->GetConsensus(), chain);
        ExpectEachRefused(params->GetConsensus(), chain);
    }
}

// Selecting a network runs the check on it, and the three release networks pass it as they are.
BOOST_AUTO_TEST_CASE(selecting_a_network_checks_it)
{
    const std::string fixture{Params().NetworkIDString()};
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        ExpectSelected(chain, fixture);
    }
    BOOST_CHECK_EQUAL(Params().NetworkIDString(), fixture);
}

// Only the network finally chosen is checked. The GUI's data directory dialog selects the network the command line
// names before the configuration is read (SelectProvisionalParams), and a refused height there must not stop it on
// its way to another network. On a release build mainnet passes and both selections succeed; on a build whose mainnet
// sets a refused height (the targeted round's badmain variant) SelectParams(main) is refused and the provisional
// selection is not. A regtest fixture, so that the fixture itself starts on that build too.
BOOST_FIXTURE_TEST_CASE(only_the_final_selection_is_checked, DashRulesRegTestSetup)
{
    bool main_refused{false};
    try {
        CheckNeverActivatedDashRules(CreateChainParams(gArgs, CBaseChainParams::MAIN)->GetConsensus(), CBaseChainParams::MAIN);
    } catch (const std::runtime_error&) {
        main_refused = true;
    }
    BOOST_TEST_MESSAGE(strprintf("mainnet's parameters refused by the check: %s", main_refused ? "yes" : "no"));
    BOOST_CHECK_NO_THROW(SelectProvisionalParams(CBaseChainParams::MAIN));
    BOOST_CHECK_EQUAL(Params().NetworkIDString(), CBaseChainParams::MAIN);
    if (main_refused) {
        BOOST_CHECK_EXCEPTION(SelectParams(CBaseChainParams::MAIN), std::runtime_error, HasReason("on main;"));
    } else {
        BOOST_CHECK_NO_THROW(SelectParams(CBaseChainParams::MAIN));
    }
    BOOST_CHECK_NO_THROW(SelectParams(CBaseChainParams::TESTNET));
    BOOST_CHECK_EQUAL(Params().NetworkIDString(), CBaseChainParams::TESTNET);
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(platform_profile_never_forms_on_mainnet_or_testnet)
{
    const std::string fixture{Params().NetworkIDString()};
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        SelectParams(chain);
        ExpectPlatformProfileNeverForms(chain);
    }
    SelectParams(fixture);
}

BOOST_FIXTURE_TEST_CASE(platform_profile_never_forms_on_devnet, DashRulesDevNetSetup)
{
    ExpectPlatformProfileNeverForms(CBaseChainParams::DEVNET);
}

BOOST_FIXTURE_TEST_CASE(devnet_leaves_them_unset_and_refuses_each, DashRulesDevNetSetup)
{
    // The fixture selected devnet (SelectParams, the check included); this selects it once more, by name.
    ExpectSelected(CBaseChainParams::DEVNET, CBaseChainParams::DEVNET);
    const auto params = CreateChainParams(gArgs, CBaseChainParams::DEVNET);
    BOOST_REQUIRE_EQUAL(params->NetworkIDString(), CBaseChainParams::DEVNET);
    ExpectNoneSet(params->GetConsensus(), CBaseChainParams::DEVNET);
    ExpectEachRefused(params->GetConsensus(), CBaseChainParams::DEVNET);
}

// The control: regtest switches them on (V19 from the first block, V20 and MN_RR at 900), moves them on request,
// and is selected.
BOOST_AUTO_TEST_CASE(regtest_keeps_them)
{
    {
        ArgsManager args;
        const auto params = CreateChainParams(args, CBaseChainParams::REGTEST);
        const auto& c = params->GetConsensus();
        BOOST_CHECK_EQUAL(c.V19Height, 1);
        BOOST_CHECK_EQUAL(c.V20Height, 900);
        BOOST_CHECK_EQUAL(c.MN_RRHeight, 900);
        BOOST_CHECK_NO_THROW(CheckNeverActivatedDashRules(c, CBaseChainParams::REGTEST));
    }
    {
        ArgsManager args;
        args.ForceSetArg("-testactivationheight", "v20@120");
        const auto params = CreateChainParams(args, CBaseChainParams::REGTEST);
        BOOST_CHECK_EQUAL(params->GetConsensus().V20Height, 120);
    }
    {
        // The DIP0024 deployment is gone: its name is refused like any unknown one.
        ArgsManager args;
        args.ForceSetArg("-testactivationheight", "dip0024@150");
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, CBaseChainParams::REGTEST), std::runtime_error,
                              HasReason("Invalid name (dip0024@150) for -testactivationheight=name@height."));
    }
    {
        // Regtest signs InstantSend with a profile that does not rotate.
        ArgsManager args;
        const auto params = CreateChainParams(args, CBaseChainParams::REGTEST);
        BOOST_CHECK(params->GetConsensus().llmqTypeDIP0024InstantSend == Consensus::LLMQType::LLMQ_TEST_INSTANTSEND);
        BOOST_CHECK(!params->GetLLMQ(static_cast<Consensus::LLMQType>(103)).has_value());
    }
    {
        // The BRR deployment is gone: its name is refused like any unknown one.
        ArgsManager args;
        args.ForceSetArg("-testactivationheight", "brr@1000");
        BOOST_CHECK_EXCEPTION(CreateChainParams(args, CBaseChainParams::REGTEST), std::runtime_error,
                              HasReason("Invalid name (brr@1000) for -testactivationheight=name@height."));
    }
}

BOOST_AUTO_TEST_SUITE_END()
