// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <evo/pose_service_metrics.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/time.h>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>
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

// The summary names a metric by its place in a table that has to be as long as
// the enum; the last entries are where a table that fell short would show.
BOOST_AUTO_TEST_CASE(summary_names_every_metric_to_the_end_of_the_table)
{
    MetricsScope scope;
    for (size_t i = 0; i < static_cast<size_t>(dsl::PerfMetric::COUNT); ++i) {
        dsl::RecordPerf(7, uint256::ONE, static_cast<dsl::PerfMetric>(i), 3);
    }
    ASSERT_DEBUG_LOG(" activation_refused=1/3ns");
    ASSERT_DEBUG_LOG(" decode_report=1/3ns");
    ASSERT_DEBUG_LOG(" duplicate_undecoded=1/3ns");
    ASSERT_DEBUG_LOG(" block_msg_wait=1/3ns");
    ASSERT_DEBUG_LOG(" duplicate_response_undecoded=1/3ns");
    dsl::LogPerfSummary();
    BOOST_CHECK(!dsl::TakePerfSnapshot().epochs[0].used); // the summary took the window with it
}

BOOST_AUTO_TEST_CASE(latency_buckets_are_log2_with_both_ends_clamped)
{
    // Bucket k is [2^k, 2^(k+1)) ns for k = 10..49; below 2^10 is bucket 10, from 2^50 up bucket 49.
    BOOST_CHECK_EQUAL(dsl::PerfBucket(0), 10u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(1023), 10u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(1024), 10u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(2047), 10u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(2048), 11u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(50'000'000), 25u); // 50 ms: 2^25 ns is 33.6 ms, 2^26 is 67.1 ms
    BOOST_CHECK_EQUAL(dsl::PerfBucket((uint64_t{1} << 49) - 1), 48u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(uint64_t{1} << 49), 49u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket((uint64_t{1} << 50) - 1), 49u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(uint64_t{1} << 50), 49u);
    BOOST_CHECK_EQUAL(dsl::PerfBucket(std::numeric_limits<uint64_t>::max()), 49u);
}

BOOST_AUTO_TEST_CASE(latency_is_kept_per_position_with_its_overflow)
{
    MetricsScope scope;
    const auto wait = dsl::PerfMetric::BLOCK_MSG_WAIT;
    const size_t index = *dsl::LatencyIndex(wait);
    dsl::RecordLatency(7, uint256::ONE, wait, 18, 3000);
    dsl::RecordLatency(7, uint256::ONE, wait, 18, 3000);
    dsl::RecordLatency(7, uint256::ONE, wait, 5, (uint64_t{1} << 50) + 1);
    dsl::RecordLatency(7, uint256::ONE, wait, dsl::PERF_POSITIONS, 3000); // no such position: totals only
    // A metric without a histogram is only counted, and allocates none.
    dsl::RecordLatency(8, uint256::ONE, dsl::PerfMetric::ACCEPTED, 3, 11);
    const auto snapshot = dsl::TakePerfSnapshot();
    const auto& slot = snapshot.epochs[0];
    BOOST_REQUIRE(slot.used && slot.epoch == 7);
    BOOST_CHECK_EQUAL(slot.values[static_cast<size_t>(wait)].count, 4u);
    BOOST_REQUIRE_EQUAL(slot.latency.size(), dsl::PERF_LATENCY_METRICS * dsl::PERF_POSITIONS);
    const auto& at18 = slot.latency[index * dsl::PERF_POSITIONS + 18];
    BOOST_CHECK_EQUAL(at18.buckets[dsl::PerfBucket(3000) - dsl::PERF_BUCKET_LOW], 2u);
    BOOST_CHECK_EQUAL(at18.overflow, 0u);
    const auto& at5 = slot.latency[index * dsl::PERF_POSITIONS + 5];
    BOOST_CHECK_EQUAL(at5.buckets[dsl::PERF_BUCKETS - 1], 1u);
    BOOST_CHECK_EQUAL(at5.overflow, 1u);
    uint64_t samples{0};
    for (const auto& histogram : slot.latency) for (const auto count : histogram.buckets) samples += count;
    BOOST_CHECK_EQUAL(samples, 3u);
    const auto& other = snapshot.epochs[1];
    BOOST_REQUIRE(other.used && other.epoch == 8);
    BOOST_CHECK_EQUAL(other.values[static_cast<size_t>(dsl::PerfMetric::ACCEPTED)].count, 1u);
    BOOST_CHECK(other.latency.empty());
    BOOST_CHECK(!dsl::LatencyIndex(dsl::PerfMetric::ACCEPTED));
    for (const auto metric : {dsl::PerfMetric::TIP_QUEUE, dsl::PerfMetric::PEERMAN_TIP_RUN, dsl::PerfMetric::DKG_TIP_START,
                              dsl::PerfMetric::DSL_TICK_RUN, dsl::PerfMetric::BLOCK_MSG_WAIT}) {
        BOOST_CHECK(dsl::LatencyIndex(metric));
    }
}

BOOST_AUTO_TEST_CASE(positions_follow_the_24_block_dkg_cycle)
{
    BOOST_CHECK_EQUAL(dsl::PerfPosition(-1), 0u);
    BOOST_CHECK_EQUAL(dsl::PerfPosition(0), 0u);
    BOOST_CHECK_EQUAL(dsl::PerfPosition(18), 18u);
    BOOST_CHECK_EQUAL(dsl::PerfPosition(23), 23u);
    BOOST_CHECK_EQUAL(dsl::PerfPosition(24), 0u);
    BOOST_CHECK_EQUAL(dsl::PerfPosition(166226), 166226u % 24);

    // A chain of 60 blocks, DSL from 48 in epochs of 24: before it nothing is filed; from it the epoch, its
    // base block and the block's position.
    std::vector<uint256> hashes(60);
    std::vector<CBlockIndex> blocks(60);
    for (size_t i = 0; i < blocks.size(); ++i) {
        hashes[i] = uint256S(strprintf("%064x", i + 1));
        blocks[i].nHeight = static_cast<int>(i);
        blocks[i].phashBlock = &hashes[i];
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].BuildSkip();
    }
    Consensus::Params consensus = Params().GetConsensus();
    consensus.nDSLEpochInterval = 24;
    consensus.nDSLActivationHeight = 48;
    BOOST_CHECK(!dsl::TipPerfKey(&blocks[47], consensus));
    BOOST_CHECK(!dsl::TipPerfKey(nullptr, consensus));
    const auto key = dsl::TipPerfKey(&blocks[50], consensus);
    BOOST_REQUIRE(key);
    BOOST_CHECK_EQUAL(key->epoch, 2u);
    BOOST_CHECK(key->base == hashes[48]);
    BOOST_CHECK_EQUAL(key->position, 2u);
    const auto boundary = dsl::TipPerfKey(&blocks[48], consensus);
    BOOST_REQUIRE(boundary);
    BOOST_CHECK(boundary->base == hashes[48]);
    BOOST_CHECK_EQUAL(boundary->position, 0u);
    consensus.nDSLEpochInterval = 0;
    BOOST_CHECK(!dsl::TipPerfKey(&blocks[50], consensus));
}

// The waits are measured on the monotonic clock: with the mockable clock standing still, a real 50 ms wait
// is still at least 50 ms, in the bucket of 50 ms, both for a measured start and for a timed scope.
BOOST_AUTO_TEST_CASE(a_latency_is_measured_on_the_monotonic_clock)
{
    MetricsScope scope;
    SetMockTime(1700000000);
    const auto start = std::chrono::steady_clock::now();
    UninterruptibleSleep(std::chrono::milliseconds{50});
    dsl::RecordLatency(3, uint256::ONE, dsl::PerfMetric::BLOCK_MSG_WAIT, 1, dsl::NanosSince(start));
    {
        dsl::LatencyTimer timer(dsl::PerfKey{3, uint256::ONE, 2}, dsl::PerfMetric::DSL_TICK_RUN);
        UninterruptibleSleep(std::chrono::milliseconds{50});
    }
    { // without a key, or with measurements off, a timer records nothing
        dsl::LatencyTimer no_key(std::nullopt, dsl::PerfMetric::DSL_TICK_RUN);
        dsl::SetPerfEnabled(false);
        dsl::LatencyTimer disabled(dsl::PerfKey{3, uint256::ONE, 2}, dsl::PerfMetric::DSL_TICK_RUN);
        dsl::SetPerfEnabled(true);
    }
    SetMockTime(0);
    const auto snapshot = dsl::TakePerfSnapshot();
    const auto& slot = snapshot.epochs[0];
    BOOST_REQUIRE(slot.used);
    for (const auto& [metric, position] : {std::pair{dsl::PerfMetric::BLOCK_MSG_WAIT, 1u}, std::pair{dsl::PerfMetric::DSL_TICK_RUN, 2u}}) {
        const auto& value = slot.values[static_cast<size_t>(metric)];
        BOOST_CHECK_EQUAL(value.count, 1u);
        BOOST_CHECK_GE(value.nanos, 50'000'000u);
        const auto& histogram = slot.latency[*dsl::LatencyIndex(metric) * dsl::PERF_POSITIONS + position];
        uint64_t at_least_50ms{0};
        for (size_t b = dsl::PerfBucket(50'000'000) - dsl::PERF_BUCKET_LOW; b < dsl::PERF_BUCKETS; ++b) at_least_50ms += histogram.buckets[b];
        BOOST_CHECK_EQUAL(at_least_50ms, 1u);
    }
}

BOOST_AUTO_TEST_CASE(summary_writes_one_histogram_line_per_metric_and_position)
{
    MetricsScope scope;
    dsl::RecordLatency(7, uint256::ONE, dsl::PerfMetric::TIP_QUEUE, 18, 2048);
    dsl::RecordLatency(7, uint256::ONE, dsl::PerfMetric::TIP_QUEUE, 18, 3000);
    dsl::RecordLatency(7, uint256::ONE, dsl::PerfMetric::TIP_QUEUE, 18, 5000);
    dsl::RecordLatency(7, uint256::ONE, dsl::PerfMetric::DKG_TIP_START, 0, uint64_t{1} << 50);
    ASSERT_DEBUG_LOG(" tip_queue=3/10048ns");
    ASSERT_DEBUG_LOG("DSL perf hist -- epoch=7 base=" + uint256::ONE.ToString() + " metric=tip_queue pos=18 n=3 over=0 buckets=11:2,12:1");
    ASSERT_DEBUG_LOG("metric=dkg_tip_start pos=0 n=1 over=1 buckets=49:1");
    dsl::LogPerfSummary();
}

BOOST_AUTO_TEST_CASE(the_queue_time_of_a_tip_event_holds_for_its_scope)
{
    BOOST_CHECK(!dsl::TipEventQueued());
    const auto outer = std::chrono::steady_clock::now();
    {
        dsl::TipEventScope scope(outer);
        BOOST_CHECK(dsl::TipEventQueued() == outer);
        const auto inner = outer + std::chrono::seconds{1};
        {
            dsl::TipEventScope nested(inner);
            BOOST_CHECK(dsl::TipEventQueued() == inner);
        }
        BOOST_CHECK(dsl::TipEventQueued() == outer);
        bool other_thread_sees_one{true};
        std::thread([&other_thread_sees_one] { other_thread_sees_one = dsl::TipEventQueued().has_value(); }).join();
        BOOST_CHECK(!other_thread_sees_one); // another thread's events are its own
    }
    BOOST_CHECK(!dsl::TipEventQueued());
}

BOOST_AUTO_TEST_CASE(the_test_process_delay_sleeps_only_when_set)
{
    dsl::SetTestProcessDelay(std::chrono::milliseconds{30});
    auto start = std::chrono::steady_clock::now();
    dsl::TestProcessDelay();
    BOOST_CHECK_GE(dsl::NanosSince(start), 30'000'000u);
    dsl::SetTestProcessDelay(std::chrono::milliseconds{-5}); // never negative: off
    start = std::chrono::steady_clock::now();
    dsl::TestProcessDelay();
    dsl::SetTestProcessDelay(std::chrono::milliseconds{0});
    dsl::TestProcessDelay();
    BOOST_CHECK_LT(dsl::NanosSince(start), 30'000'000u);
}
BOOST_AUTO_TEST_SUITE_END()
