// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_metrics.h>
#include <test/util/setup_common.h>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_metrics_tests, BasicTestingSetup)
namespace {
struct MetricsScope {
    bool previous{dsl::PerfEnabled()};
    MetricsScope() { dsl::TakePerfSnapshot(); dsl::SetPerfEnabled(true); }
    ~MetricsScope() { dsl::SetPerfEnabled(previous); dsl::TakePerfSnapshot(); }
};
}

BOOST_AUTO_TEST_CASE(disabled_and_bounded_by_epoch_and_base)
{
    MetricsScope scope;
    dsl::SetPerfEnabled(false);
    dsl::RecordPerf(1, uint256::ONE, dsl::PerfMetric::ACCEPTED);
    BOOST_CHECK(!dsl::TakePerfSnapshot().epochs[0].used);
    dsl::SetPerfEnabled(true);
    for (size_t i = 0; i < dsl::PERF_EPOCH_SLOTS + 50; ++i) {
        dsl::RecordPerf(static_cast<uint32_t>(i), uint256::ONE, dsl::PerfMetric::ACCEPTED, 7);
    }
    // A full window still updates existing buckets; new keys cannot grow RAM.
    dsl::RecordPerf(0, uint256::ONE, dsl::PerfMetric::ACCEPTED, 11);
    dsl::RecordPerf(0, uint256::TWO, dsl::PerfMetric::ACCEPTED);
    const auto snapshot = dsl::TakePerfSnapshot();
    BOOST_CHECK_EQUAL(snapshot.overflow, 51u);
    BOOST_CHECK_EQUAL(std::count_if(snapshot.epochs.begin(), snapshot.epochs.end(),
                                  [](const auto& e) { return e.used; }), dsl::PERF_EPOCH_SLOTS);
    const auto& value = snapshot.epochs[0].values[static_cast<size_t>(dsl::PerfMetric::ACCEPTED)];
    BOOST_CHECK_EQUAL(value.count, 2u);
    BOOST_CHECK_EQUAL(value.nanos, 18u);
    BOOST_CHECK(!dsl::TakePerfSnapshot().epochs[0].used);
}

BOOST_AUTO_TEST_CASE(parallel_updates_are_not_lost)
{
    MetricsScope scope;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([] {
        for (int j = 0; j < 1000; ++j) dsl::RecordPerf(42, uint256::ONE, dsl::PerfMetric::WIRE_REPORT, 3);
    });
    for (auto& worker : workers) worker.join();
    const auto snapshot = dsl::TakePerfSnapshot();
    const auto& value = snapshot.epochs[0].values[static_cast<size_t>(dsl::PerfMetric::WIRE_REPORT)];
    BOOST_CHECK_EQUAL(value.count, 4000u);
    BOOST_CHECK_EQUAL(value.nanos, 12000u);
    BOOST_CHECK_EQUAL(snapshot.overflow, 0u);
}
BOOST_AUTO_TEST_SUITE_END()
