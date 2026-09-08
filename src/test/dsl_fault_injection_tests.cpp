// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <evo/pose_service_faults.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_FIXTURE_TEST_SUITE(dsl_fault_injection_tests, BasicTestingSetup)

namespace {
std::unique_ptr<const CChainParams> ParamsFor(const std::string& chain)
{
    if (chain == CBaseChainParams::DEVNET) {
        // A devnet's parameters read their name from the global arguments
        // (GetDevNetName asserts on it), so this follows the pattern of
        // block_reward_reallocation_tests: set, build, remove.
        gArgs.SoftSetBoolArg("-devnet", true);
        auto params = CreateChainParams(gArgs, chain);
        gArgs.ForceRemoveArg("devnet");
        return params;
    }
    return CreateChainParams(ArgsManager{}, chain);
}
} // namespace

// The gate is the acceptance criterion of this whole change: on mainnet the
// injector cannot be armed from the configuration, and because the injector is
// then never enabled, not from the RPC either. Testnet is refused for the same
// reason -- it is a public network with real operators on it.
BOOST_AUTO_TEST_CASE(startup_refuses_fault_injection_outside_devnet_and_regtest)
{
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        const auto params = ParamsFor(chain);
        BOOST_CHECK(!dsl::FaultInjectionAllowedOn(*params));
        const auto refusal = dsl::FaultInjectionRefusal(*params, /*requested=*/true);
        BOOST_REQUIRE(refusal.has_value());
        BOOST_CHECK(refusal->find("-enablefaultinjection") != std::string::npos);
        BOOST_CHECK(refusal->find(chain) != std::string::npos);
        // not asking for it is never an error, on any chain
        BOOST_CHECK(!dsl::FaultInjectionRefusal(*params, /*requested=*/false).has_value());
    }
    for (const auto& chain : {CBaseChainParams::DEVNET, CBaseChainParams::REGTEST}) {
        const auto params = ParamsFor(chain);
        BOOST_CHECK(dsl::FaultInjectionAllowedOn(*params));
        BOOST_CHECK(!dsl::FaultInjectionRefusal(*params, /*requested=*/true).has_value());
        BOOST_CHECK(!dsl::FaultInjectionRefusal(*params, /*requested=*/false).has_value());
    }
}

// A disabled injector is inert in every direction: it stores nothing, lists
// nothing and answers no fault -- so an RPC reaching it on a chain where the
// gate stayed closed can only ever report "disabled".
BOOST_AUTO_TEST_CASE(disabled_injector_is_inert)
{
    dsl::CFaultInjector off(/*enabled=*/false);
    BOOST_CHECK(!off.Enabled());
    BOOST_CHECK(!off.Set(dsl::FaultKind::RESPONSE_DROP, 100, 200, 0, "scenario").has_value());
    BOOST_CHECK(off.List(100).empty());
    BOOST_CHECK(!off.Active(dsl::FaultKind::RESPONSE_DROP, 100).has_value());
    BOOST_CHECK_EQUAL(off.Clear(), 0U);
    BOOST_CHECK(!off.Clear(1));
}

BOOST_AUTO_TEST_CASE(faults_expire_by_height_and_the_expiry_block_is_already_clean)
{
    dsl::CFaultInjector on(/*enabled=*/true);
    BOOST_CHECK(on.Enabled());
    const auto fault = on.Set(dsl::FaultKind::REPORT_DROP, /*currentHeight=*/100, /*expiryHeight=*/103, 0, "s1");
    BOOST_REQUIRE(fault.has_value());
    BOOST_CHECK_EQUAL(fault->id, 1U);
    BOOST_CHECK_EQUAL(fault->setAtHeight, 100);
    BOOST_CHECK_EQUAL(fault->expiryHeight, 103);
    BOOST_CHECK_EQUAL(fault->scenarioId, "s1");

    for (int h : {100, 101, 102}) {
        BOOST_CHECK_MESSAGE(on.Active(dsl::FaultKind::REPORT_DROP, h).has_value(), "active at " << h);
        BOOST_CHECK_EQUAL(on.List(h).size(), 1U);
    }
    // the expiry height itself, and anything past it, is clean
    BOOST_CHECK(!on.Active(dsl::FaultKind::REPORT_DROP, 103).has_value());
    BOOST_CHECK(on.List(103).empty());
    BOOST_CHECK(on.List(10'000).empty());
    // a different kind was never active
    BOOST_CHECK(!on.Active(dsl::FaultKind::RESPONSE_DROP, 101).has_value());

    // the sweep removes exactly the expired ones, and reports the count
    BOOST_CHECK_EQUAL(on.Expire(102), 0U);
    BOOST_CHECK_EQUAL(on.Expire(103), 1U);
    BOOST_CHECK_EQUAL(on.Expire(103), 0U);
}

BOOST_AUTO_TEST_CASE(set_refuses_what_could_never_act)
{
    dsl::CFaultInjector on(/*enabled=*/true);
    // expiry at or below the current height: dead on arrival
    BOOST_CHECK(!on.Set(dsl::FaultKind::RESPONSE_DROP, 100, 100, 0, "s").has_value());
    BOOST_CHECK(!on.Set(dsl::FaultKind::RESPONSE_DROP, 100, 99, 0, "s").has_value());
    // no scenario: nothing to attribute the observation to
    BOOST_CHECK(!on.Set(dsl::FaultKind::RESPONSE_DROP, 100, 101, 0, "").has_value());
    // a delay of zero blocks is not a delay
    BOOST_CHECK(!on.Set(dsl::FaultKind::RESPONSE_DELAY, 100, 101, 0, "s").has_value());
    BOOST_CHECK(!on.Set(dsl::FaultKind::REPORT_DELAY, 100, 101, 0, "s").has_value());
    BOOST_CHECK(on.Set(dsl::FaultKind::RESPONSE_DELAY, 100, 101, 2, "s").has_value());
    // the enum sentinel is not a kind
    BOOST_CHECK(!on.Set(dsl::FaultKind::_COUNT, 100, 101, 0, "s").has_value());
    // exactly one survived: the valid delay
    BOOST_CHECK_EQUAL(on.List(100).size(), 1U);
}

BOOST_AUTO_TEST_CASE(ids_are_unique_and_clear_targets_one_or_all)
{
    dsl::CFaultInjector on(/*enabled=*/true);
    const auto a = on.Set(dsl::FaultKind::RESPONSE_DROP, 10, 50, 0, "a");
    const auto b = on.Set(dsl::FaultKind::RESPONSE_DROP, 10, 60, 0, "b");
    const auto c = on.Set(dsl::FaultKind::COMMITMENT_SKIP, 10, 70, 0, "c");
    BOOST_REQUIRE(a && b && c);
    BOOST_CHECK(a->id != b->id && b->id != c->id);
    // the oldest active of a kind is the one the network layer gets
    BOOST_CHECK_EQUAL(on.Active(dsl::FaultKind::RESPONSE_DROP, 10)->id, a->id);
    BOOST_CHECK(on.Clear(a->id));
    BOOST_CHECK(!on.Clear(a->id));
    BOOST_CHECK_EQUAL(on.Active(dsl::FaultKind::RESPONSE_DROP, 10)->id, b->id);
    BOOST_CHECK_EQUAL(on.List(10).size(), 2U);
    BOOST_CHECK_EQUAL(on.Clear(), 2U);
    BOOST_CHECK(on.List(10).empty());
    // ids are never reused, so a stale reference cannot hit a newer fault
    const auto d = on.Set(dsl::FaultKind::RESPONSE_DROP, 10, 50, 0, "d");
    BOOST_REQUIRE(d);
    BOOST_CHECK(d->id > c->id);
}

BOOST_AUTO_TEST_CASE(apply_counts_what_a_fault_did_and_active_does_not)
{
    dsl::CFaultInjector on(/*enabled=*/true);
    const auto fault = on.Set(dsl::FaultKind::RESPONSE_DROP, 10, 20, 0, "s");
    BOOST_REQUIRE(fault.has_value());
    BOOST_CHECK_EQUAL(fault->hits, 0U);
    // looking is free
    BOOST_CHECK(on.Active(dsl::FaultKind::RESPONSE_DROP, 10).has_value());
    BOOST_CHECK_EQUAL(on.List(10).front().hits, 0U);
    // acting is counted, on the stored fault, and reported by Apply itself
    BOOST_CHECK_EQUAL(on.Apply(dsl::FaultKind::RESPONSE_DROP, 10)->hits, 1U);
    BOOST_CHECK_EQUAL(on.Apply(dsl::FaultKind::RESPONSE_DROP, 19)->hits, 2U);
    BOOST_CHECK_EQUAL(on.List(19).front().hits, 2U);
    // past expiry nothing acts, and nothing is counted
    BOOST_CHECK(!on.Apply(dsl::FaultKind::RESPONSE_DROP, 20).has_value());
    // a different kind never matched
    BOOST_CHECK(!on.Apply(dsl::FaultKind::REPORT_DROP, 10).has_value());
    BOOST_CHECK_EQUAL(on.List(10).front().hits, 2U);
    dsl::CFaultInjector off(/*enabled=*/false);
    BOOST_CHECK(!off.Apply(dsl::FaultKind::RESPONSE_DROP, 10).has_value());
}

BOOST_AUTO_TEST_CASE(kind_names_round_trip_and_unknown_names_are_refused)
{
    for (const auto kind : dsl::AllFaultKinds()) {
        const auto name = dsl::FaultKindName(kind);
        BOOST_CHECK(!name.empty());
        BOOST_CHECK(name != "unknown");
        const auto back = dsl::FaultKindFromName(name);
        BOOST_REQUIRE(back.has_value());
        BOOST_CHECK(*back == kind);
    }
    BOOST_CHECK_EQUAL(dsl::AllFaultKinds().size(), 5U);
    BOOST_CHECK(!dsl::FaultKindFromName("").has_value());
    BOOST_CHECK(!dsl::FaultKindFromName("drop").has_value());
    BOOST_CHECK(!dsl::FaultKindFromName("RESPONSE-DROP").has_value());
    BOOST_CHECK_EQUAL(dsl::FaultKindName(dsl::FaultKind::_COUNT), "unknown");
}

BOOST_AUTO_TEST_SUITE_END()
