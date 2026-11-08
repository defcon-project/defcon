// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <pos/stakingpace.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <string>

BOOST_FIXTURE_TEST_SUITE(pos_stakingpace_tests, BasicTestingSetup)

namespace {
std::unique_ptr<const CChainParams> ParamsFor(const std::string& chain)
{
    if (chain == CBaseChainParams::DEVNET) {
        // A devnet's parameters read their name from the global arguments
        // (GetDevNetName asserts on it): set, build, remove, as in
        // dsl_fault_injection_tests.
        gArgs.SoftSetBoolArg("-devnet", true);
        auto params = CreateChainParams(gArgs, chain);
        gArgs.ForceRemoveArg("devnet");
        return params;
    }
    return CreateChainParams(ArgsManager{}, chain);
}

// ArgsManager holds a mutex and cannot be copied: each check builds its own.
void SetPace(ArgsManager& args, const std::string& value)
{
    args.ForceSetArg(pos::STAKING_POLL_ARG, value);
}
} // namespace

// The acceptance criterion of this facility: the pace of a public network
// cannot be changed from the configuration. Startup is refused there, and the
// interval stays at the production pause even if the gate were bypassed.
BOOST_AUTO_TEST_CASE(public_chains_refuse_the_argument_and_keep_the_production_pace)
{
    const std::chrono::milliseconds production{pos::DEFAULT_STAKING_POLL_MS};
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        const auto params = ParamsFor(chain);
        BOOST_CHECK(!pos::StakingPaceAllowedOn(*params));
        for (const auto& value : {"100", "2500", "50"}) {
            ArgsManager args;
            SetPace(args, value);
            const auto refusal = pos::StakingPaceRefusal(*params, args);
            BOOST_REQUIRE(refusal.has_value());
            BOOST_CHECK(refusal->find(pos::STAKING_POLL_ARG) != std::string::npos);
            BOOST_CHECK(refusal->find(chain) != std::string::npos);
            BOOST_CHECK(pos::StakingPollInterval(*params, args) == production);
        }
        // not setting it is never an error, on any chain
        ArgsManager unset;
        BOOST_CHECK(!pos::StakingPaceRefusal(*params, unset).has_value());
        BOOST_CHECK(pos::StakingPollInterval(*params, unset) == production);
    }
}

BOOST_AUTO_TEST_CASE(test_chains_accept_the_range_and_nothing_outside_it)
{
    for (const auto& chain : {CBaseChainParams::DEVNET, CBaseChainParams::REGTEST}) {
        const auto params = ParamsFor(chain);
        BOOST_CHECK(pos::StakingPaceAllowedOn(*params));

        ArgsManager unset;
        BOOST_CHECK(!pos::StakingPaceRefusal(*params, unset).has_value());
        BOOST_CHECK(pos::StakingPollInterval(*params, unset) == std::chrono::milliseconds{pos::DEFAULT_STAKING_POLL_MS});

        for (const auto& [value, ms] : {std::pair{"50", 50}, std::pair{"100", 100}, std::pair{"2500", 2500}}) {
            ArgsManager args;
            SetPace(args, value);
            BOOST_CHECK_MESSAGE(!pos::StakingPaceRefusal(*params, args).has_value(), chain << " " << value);
            BOOST_CHECK(pos::StakingPollInterval(*params, args) == std::chrono::milliseconds{ms});
        }
        // Out of range, negative, zero or with no leading number: refused at startup, never used.
        for (const auto& value : {"49", "2501", "0", "-100", "fast", ""}) {
            ArgsManager args;
            SetPace(args, value);
            const auto refusal = pos::StakingPaceRefusal(*params, args);
            BOOST_REQUIRE_MESSAGE(refusal.has_value(), chain << " '" << value << "'");
            BOOST_CHECK(refusal->find("between") != std::string::npos);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
