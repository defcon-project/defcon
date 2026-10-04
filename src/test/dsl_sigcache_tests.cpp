// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_sigcache.h>
#include <evo/pose_service_sentinels.h>
#include <evo/pose_service_metrics.h>
#include <hash.h>
#include <test/util/setup_common.h>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_sigcache_tests, BasicTestingSetup)
namespace {
struct MetricsScope {
    bool old{dsl::PerfEnabled()};
    MetricsScope() { dsl::TakePerfSnapshot(); dsl::SetPerfEnabled(true); }
    ~MetricsScope() { dsl::SetPerfEnabled(old); dsl::TakePerfSnapshot(); }
};
uint64_t Calls(const dsl::PerfSnapshot& snapshot, dsl::PerfMetric metric)
{
    uint64_t count{0};
    for (const auto& slot : snapshot.epochs) count += slot.values[static_cast<size_t>(metric)].count;
    return count;
}
dsl::CPoSeServiceReport Report(const CBLSSecretKey& key, uint32_t epoch = 42)
{
    dsl::CPoSeServiceReport report;
    report.nEpoch = epoch;
    report.targetProTxHash = uint256::ONE;
    report.sentinelProTxHash = uint256::TWO;
    report.status = static_cast<uint8_t>(dsl::ServiceStatus::ONLINE);
    report.Sign(key, uint256::ONE);
    return report;
}
}

BOOST_AUTO_TEST_CASE(hits_bind_signature_message_base_key_and_modern_scheme)
{
    MetricsScope scope;
    dsl::CReportSignatureCache cache;
    CBLSSecretKey key, other;
    key.MakeNewKey(); other.MakeNewKey();
    const auto pk = key.GetPublicKey();
    const auto good = Report(key);
    BOOST_REQUIRE(cache.Verify(good, pk, uint256::ONE));
    BOOST_REQUIRE(cache.Verify(good, pk, uint256::ONE));
    auto snapshot = dsl::TakePerfSnapshot();
    BOOST_CHECK_EQUAL(Calls(snapshot, dsl::PerfMetric::BLS_VERIFY), 1u);
    BOOST_CHECK_EQUAL(Calls(snapshot, dsl::PerfMetric::CACHE_HIT), 1u);
    const auto reject = [&](const auto& report, const auto& public_key, const auto& base) {
        BOOST_CHECK(!cache.Verify(report, public_key, base));
    };
    auto wrong = good;
    wrong.sig = other.Sign(good.GetSignHash(uint256::ONE), false);
    reject(wrong, pk, uint256::ONE);
    reject(wrong, pk, uint256::ONE); // failure is not cached
    snapshot = dsl::TakePerfSnapshot();
    BOOST_CHECK_EQUAL(Calls(snapshot, dsl::PerfMetric::BLS_VERIFY), 2u);
    BOOST_CHECK_EQUAL(Calls(snapshot, dsl::PerfMetric::CACHE_HIT), 0u);
    wrong = good; ++wrong.nEpoch; reject(wrong, pk, uint256::ONE);
    wrong = good; wrong.status = static_cast<uint8_t>(dsl::ServiceStatus::MISSED); reject(wrong, pk, uint256::ONE);
    wrong = good; wrong.targetProTxHash = uint256::TWO; reject(wrong, pk, uint256::ONE);
    wrong = good; wrong.sentinelProTxHash = uint256::ONE; reject(wrong, pk, uint256::ONE);
    reject(good, pk, uint256::TWO);
    reject(good, other.GetPublicKey(), uint256::ONE);
    wrong = good; wrong.sig = CBLSSignature(); reject(wrong, pk, uint256::ONE);
    reject(good, CBLSPublicKey(), uint256::ONE);
    struct RestoreScheme {
        bool old{bls::bls_legacy_scheme.load()};
        ~RestoreScheme() { bls::bls_legacy_scheme.store(old); }
    } restore;
    for (const bool legacy : {false, true}) {
        bls::bls_legacy_scheme.store(legacy);
        BOOST_CHECK(cache.Verify(good, pk, uint256::ONE));
        // Also exercise a cold entry while the global scheme is legacy.
        dsl::CReportSignatureCache cold(2);
        BOOST_CHECK(cold.Verify(good, pk, uint256::ONE));
        wrong = good; wrong.sig = key.Sign(good.GetSignHash(uint256::ONE), true);
        reject(wrong, pk, uint256::ONE);
    }
    BOOST_CHECK(cache.Verify(good, pk, uint256::ONE));
}

BOOST_AUTO_TEST_CASE(eviction_falls_back_to_verification_with_fixed_capacity)
{
    MetricsScope scope;
    dsl::CReportSignatureCache small(8), clamped(1000000);
    BOOST_CHECK_EQUAL(small.Capacity(), 8u);
    BOOST_CHECK_EQUAL(clamped.Capacity(), dsl::CReportSignatureCache::MAX_ENTRIES);
    CBLSSecretKey key; key.MakeNewKey();
    const auto pk = key.GetPublicKey();
    std::vector<dsl::CPoSeServiceReport> reports;
    for (uint32_t i = 0; i < 64; ++i) {
        auto report = Report(key);
        CHashWriter writer(SER_GETHASH, 0);
        writer << i;
        report.targetProTxHash = writer.GetHash();
        report.Sign(key, uint256::ONE);
        reports.push_back(report);
        BOOST_REQUIRE(small.Verify(reports.back(), pk, uint256::ONE));
    }
    dsl::TakePerfSnapshot();
    for (const auto& report : reports) BOOST_CHECK(small.Verify(report, pk, uint256::ONE));
    const auto snapshot = dsl::TakePerfSnapshot();
    // Do not assume which particular entry the bounded cache evicted.
    BOOST_CHECK(Calls(snapshot, dsl::PerfMetric::BLS_VERIFY) > 0u);
    BOOST_CHECK_EQUAL(small.Capacity(), 8u);
}

BOOST_AUTO_TEST_CASE(parallel_valid_and_invalid_checks_do_not_poison_success)
{
    MetricsScope scope;
    dsl::CReportSignatureCache cache(8);
    CBLSSecretKey key, other; key.MakeNewKey(); other.MakeNewKey();
    const auto pk = key.GetPublicKey();
    const auto good = Report(key);
    auto bad = good;
    bad.sig = other.Sign(good.GetSignHash(uint256::ONE), false);
    std::atomic<bool> correct{true};
    dsl::SetPerfEnabled(false); // concurrency check independent of the metric mutex
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
        for (int j = 0; j < 20; ++j) {
            if (!cache.Verify(good, pk, uint256::ONE) || cache.Verify(bad, pk, uint256::ONE)) correct = false;
        }
    });
    for (auto& worker : workers) worker.join();
    BOOST_CHECK(correct.load());
    dsl::TakePerfSnapshot();
    dsl::SetPerfEnabled(true);
    BOOST_CHECK(cache.Verify(good, pk, uint256::ONE));
    const auto snapshot = dsl::TakePerfSnapshot();
    BOOST_CHECK_EQUAL(Calls(snapshot, dsl::PerfMetric::BLS_VERIFY), 0u);
    BOOST_CHECK_EQUAL(Calls(snapshot, dsl::PerfMetric::CACHE_HIT), 1u);
}
BOOST_AUTO_TEST_SUITE_END()
