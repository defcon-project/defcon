// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// What one epoch of sentinel assignment costs, at network sizes the layer is
// meant to serve. Times the PRODUCTION functions, not a model of them.

#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <evo/pose_service_sentinels.h>

#include <hash.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

uint256 TH(uint64_t a, uint64_t b, std::string_view tag)
{
    CHashWriter w{SER_NETWORK, 0};
    w << a << b << std::string{tag};
    return w.GetHash();
}

CDeterministicMNList BuildList(size_t population)
{
    CDeterministicMNList list{TH(0, population, "list-block"), 1, static_cast<uint32_t>(population)};
    for (size_t i = 0; i < population; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        dmn->proTxHash = TH(0, i, "protx");
        dmn->collateralOutpoint = COutPoint{TH(0, i, "collateral"), 0};
        auto st = std::make_shared<CDeterministicMNState>();
        st->nRegisteredHeight = 1;
        st->UpdateConfirmedHash(dmn->proTxHash, TH(0, i, "confirmed"));
        uint160 owner;
        std::copy_n(dmn->proTxHash.begin(), owner.size(), owner.begin());
        st->keyIDOwner = CKeyID{owner};
        dmn->pdmnState = st;
        list.AddMN(dmn);
    }
    return list;
}

double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(dsl_perf_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sentinel_assignment_cost)
{
    const Consensus::Params defaults{};
    const size_t count = static_cast<size_t>(defaults.nDSLSentinelCount);

    for (const size_t population : {150u, 500u, 1500u, 5000u}) {
        const auto list = BuildList(population);
        const uint256 epochHash = TH(1, population, "epoch");
        std::vector<uint256> protx;
        protx.reserve(population);
        list.ForEachMN(false, [&](const auto& dmn) { protx.push_back(dmn.proTxHash); });

        // one assignment: what AddReport pays for EVERY incoming report
        auto t0 = std::chrono::steady_clock::now();
        constexpr int kReps = 20;
        for (int i = 0; i < kReps; ++i) {
            auto s = dsl::CalcSentinelsForMN(list, protx[i % population], epochHash, count);
            BOOST_REQUIRE_EQUAL(s.size(), count);
        }
        const double one_ms = MsSince(t0) / kReps;

        // one node's own per-epoch work: the inverse assignment over the list
        t0 = std::chrono::steady_clock::now();
        const auto targets = dsl::GetProbeTargetsForSentinel(list, protx[0], epochHash, count);
        const double inverse_ms = MsSince(t0);

        BOOST_TEST_MESSAGE("N=" << population
                           << " | CalcSentinelsForMN " << one_ms << " ms/call"
                           << " | GetProbeTargetsForSentinel " << inverse_ms << " ms"
                           << " (targets=" << targets.size() << ")"
                           << " | commitment build (N selections) ~"
                           << (one_ms * double(population) / 1000.0) << " s"
                           << " | report flood (" << count << "N selections) ~"
                           << (one_ms * double(count) * double(population) / 1000.0) << " s");
    }
}

BOOST_AUTO_TEST_SUITE_END()
