// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/pose_service.h>
#include <evo/pose_service_sentinels.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_store.h>
#include <hash.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_aggregation_tests, BasicTestingSetup)

namespace {
uint256 TaggedHash(uint64_t a, uint64_t b, std::string_view tag)
{
    CHashWriter w(SER_GETHASH, 0);
    w << a << b << std::string{tag};
    return w.GetHash();
}

struct Fixture {
    CDeterministicMNList list{uint256(), 1, 0};
    std::map<uint256, CBLSSecretKey> opKeys; // proTxHash -> operator secret key
    uint256 epoch = TaggedHash(500, 0, "epoch");
};

Fixture MakeFixture(size_t n)
{
    Fixture fx;
    fx.list = CDeterministicMNList(uint256(), /*height=*/1, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        dmn->proTxHash = TaggedHash(i, 0, "protx");
        dmn->collateralOutpoint = COutPoint(dmn->proTxHash, 0);
        auto st = std::make_shared<CDeterministicMNState>();
        st->nRegisteredHeight = 1;
        st->UpdateConfirmedHash(dmn->proTxHash, TaggedHash(i, 0, "confirmed"));
        CBLSSecretKey sk;
        sk.MakeNewKey();
        st->pubKeyOperator.Set(sk.GetPublicKey(), /*specificLegacyScheme=*/false);
        fx.opKeys[dmn->proTxHash] = sk;
        CKeyID owner;
        std::memcpy(owner.begin(), dmn->proTxHash.begin(), owner.size());
        st->keyIDOwner = owner;
        dmn->pdmnState = st;
        fx.list.AddMN(dmn);
    }
    return fx;
}

dsl::CPoSeServiceReport SignedReport(uint32_t epoch, const uint256& target, const uint256& sentinel,
                                     dsl::ServiceStatus status, const CBLSSecretKey& key)
{
    dsl::CPoSeServiceReport r;
    r.nEpoch = epoch;
    r.targetProTxHash = target;
    r.sentinelProTxHash = sentinel;
    r.status = static_cast<uint8_t>(status);
    r.Sign(key, TaggedHash(500, 0, "epoch")); // the fixture's epoch base
    return r;
}

// canonical index of a proTxHash in the sorted MN list
size_t CanonicalIndex(const CDeterministicMNList& list, const uint256& protx)
{
    std::vector<uint256> order;
    list.ForEachMN(false, [&](const auto& dmn) { order.push_back(dmn.proTxHash); });
    std::sort(order.begin(), order.end());
    return static_cast<size_t>(std::find(order.begin(), order.end(), protx) - order.begin());
}
} // namespace

BOOST_AUTO_TEST_CASE(five_agreeing_missed_sets_the_bit)
{
    auto fx = MakeFixture(30);
    Consensus::Params params; // defaults: 7 sentinels, agree 5
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    const uint32_t epoch = 500;

    // four MISSED reports: below the agreement threshold -> not set
    std::vector<dsl::CPoSeServiceReport> reports;
    for (size_t k = 0; k < 4; ++k) {
        reports.push_back(SignedReport(epoch, target, sentinels[k], dsl::ServiceStatus::MISSED,
                                       fx.opKeys[sentinels[k]]));
    }
    auto c4 = dsl::BuildServiceCommitment(epoch, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                          uint256::ONE, reports, fx.list, params);
    BOOST_CHECK(!c4.missed[CanonicalIndex(fx.list, target)]);

    // the fifth pushes it over the threshold -> set
    reports.push_back(SignedReport(epoch, target, sentinels[4], dsl::ServiceStatus::MISSED,
                                   fx.opKeys[sentinels[4]]));
    auto c5 = dsl::BuildServiceCommitment(epoch, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                          uint256::ONE, reports, fx.list, params);
    BOOST_CHECK(c5.missed[CanonicalIndex(fx.list, target)]);
    // no other masternode is marked
    BOOST_CHECK_EQUAL(c5.CountMissed(), 1);
}

BOOST_AUTO_TEST_CASE(only_assigned_verified_unique_reports_count)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(7, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    const uint32_t epoch = 500;
    const size_t idx = CanonicalIndex(fx.list, target);

    std::vector<dsl::CPoSeServiceReport> reports;
    // four honest MISSED from assigned sentinels
    for (size_t k = 0; k < 4; ++k) {
        reports.push_back(SignedReport(epoch, target, sentinels[k], dsl::ServiceStatus::MISSED,
                                       fx.opKeys[sentinels[k]]));
    }
    // a duplicate of sentinel 0 must not count twice
    reports.push_back(SignedReport(epoch, target, sentinels[0], dsl::ServiceStatus::MISSED,
                                   fx.opKeys[sentinels[0]]));
    // a report from a NON-assigned masternode must be ignored
    const uint256 outsider = TaggedHash(29, 0, "protx");
    BOOST_REQUIRE(std::find(sentinels.begin(), sentinels.end(), outsider) == sentinels.end());
    reports.push_back(SignedReport(epoch, target, outsider, dsl::ServiceStatus::MISSED,
                                   fx.opKeys[outsider]));
    // an assigned sentinel but signed with the WRONG key must be ignored
    reports.push_back(SignedReport(epoch, target, sentinels[5], dsl::ServiceStatus::MISSED,
                                   fx.opKeys[outsider]));
    auto c = dsl::BuildServiceCommitment(epoch, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                         uint256::ONE, reports, fx.list, params);
    // still only four valid MISSED -> not set
    BOOST_CHECK(!c.missed[idx]);

    // one more genuine assigned MISSED reaches five -> set
    reports.push_back(SignedReport(epoch, target, sentinels[4], dsl::ServiceStatus::MISSED,
                                   fx.opKeys[sentinels[4]]));
    auto c2 = dsl::BuildServiceCommitment(epoch, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                          uint256::ONE, reports, fx.list, params);
    BOOST_CHECK(c2.missed[idx]);
}

// A wrong epoch on the reports is ignored (they belong to another epoch).
BOOST_AUTO_TEST_CASE(reports_from_another_epoch_are_ignored)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(9, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);

    std::vector<dsl::CPoSeServiceReport> reports;
    for (size_t k = 0; k < 6; ++k) {
        reports.push_back(SignedReport(/*epoch=*/499, target, sentinels[k], dsl::ServiceStatus::MISSED,
                                       fx.opKeys[sentinels[k]]));
    }
    auto c = dsl::BuildServiceCommitment(/*nEpoch=*/500, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                         uint256::ONE, reports, fx.list, params);
    BOOST_CHECK_EQUAL(c.CountMissed(), 0);
}

// Quorum members with the same report pool must sign the same hash, and the
// hash must be over the transaction with the signature still zeroed -- that is
// what consensus recomputes when it verifies the mined commitment.
BOOST_AUTO_TEST_CASE(commitment_tx_candidate_is_deterministic_and_unsigned)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch,
                                                   static_cast<size_t>(params.nDSLSentinelCount));
    std::vector<dsl::CPoSeServiceReport> reports;
    for (size_t k = 0; k < 5; ++k) {
        reports.push_back(SignedReport(500, target, sentinels[k], dsl::ServiceStatus::MISSED,
                                       fx.opKeys[sentinels[k]]));
    }

    const auto a = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                                 uint256::ONE, reports, fx.list, params);
    const auto b = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                                 uint256::ONE, reports, fx.list, params);
    BOOST_CHECK(a.msgHash == b.msgHash);
    BOOST_CHECK(a.commitment.missed == b.commitment.missed);
    BOOST_CHECK_EQUAL(a.commitment.CountMissed(), 1);
    BOOST_CHECK(!a.commitment.quorumSig.IsValid()); // unsigned until the quorum answers
    BOOST_CHECK_EQUAL(a.tx.nType, TRANSACTION_POSE_SERVICE_COMMITMENT);
    BOOST_CHECK_EQUAL(a.tx.nVersion, 3);
    BOOST_CHECK(a.tx.vin.empty() && a.tx.vout.empty());

    // a different report set signs a different hash
    reports.pop_back();
    const auto c = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                                 uint256::ONE, reports, fx.list, params);
    BOOST_CHECK(c.msgHash != a.msgHash);
    BOOST_CHECK_EQUAL(c.commitment.CountMissed(), 0);
}

// Real signed report/store/build workload. The same snapshot is reused with
// diagnostics off and on; assignment-only extrapolations miss repeated BLS work.
BOOST_AUTO_TEST_CASE(repeated_candidate_cost_and_measurement_equivalence)
{
    struct RestoreMetrics {
        bool old{dsl::PerfEnabled()};
        ~RestoreMetrics() { dsl::SetPerfEnabled(old); dsl::TakePerfSnapshot(); }
    } restore;
    for (const size_t population : {152u, 220u}) {
        auto fx = MakeFixture(population);
        Consensus::Params params;
        std::vector<dsl::CPoSeServiceReport> reports;
        fx.list.ForEachMN(false, [&](const auto& dmn) {
            const auto sentinels = dsl::CalcSentinelsForMN(fx.list, dmn.proTxHash, fx.epoch, 7);
            for (const auto& sentinel : sentinels) {
                reports.push_back(SignedReport(500, dmn.proTxHash, sentinel,
                                               dsl::ServiceStatus::ONLINE, fx.opKeys.at(sentinel)));
            }
        });
        dsl::SetPerfEnabled(false);
        dsl::CServiceReportStore store;
        store.SetCurrentEpoch(500);
        for (const auto& report : reports) BOOST_REQUIRE(store.AddReport(report, fx.list, fx.epoch, params));
        const auto pooled = store.GetReportsForEpoch(500);
        BOOST_REQUIRE_EQUAL(pooled.size(), population * 7);
        const auto build = [&] {
            return dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60,
                                                 uint256::ONE, pooled, fx.list, params);
        };
        const auto expected = build();
        const auto off_start = std::chrono::steady_clock::now();
        for (int i = 0; i < 3; ++i) BOOST_CHECK(build().msgHash == expected.msgHash);
        const auto off_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - off_start).count();
        dsl::TakePerfSnapshot();
        dsl::SetPerfEnabled(true);
        const auto on_start = std::chrono::steady_clock::now();
        for (int i = 0; i < 3; ++i) {
            const auto actual = build();
            BOOST_CHECK(actual.msgHash == expected.msgHash);
            BOOST_CHECK(actual.commitment.missed == expected.commitment.missed);
            BOOST_CHECK(actual.commitment.observed == expected.commitment.observed);
            BOOST_CHECK(CTransaction(actual.tx).GetHash() == CTransaction(expected.tx).GetHash());
        }
        const auto on_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - on_start).count();
        const auto snapshot = dsl::TakePerfSnapshot();
        const auto& values = snapshot.epochs[0].values;
        BOOST_CHECK_EQUAL(values[static_cast<size_t>(dsl::PerfMetric::VERIFY_REQUEST)].count, population * 7 * 3);
        BOOST_CHECK_EQUAL(values[static_cast<size_t>(dsl::PerfMetric::COMMITMENT_BUILD)].count, 3u);
        BOOST_TEST_MESSAGE("N=" << population << " reports=" << pooled.size()
                           << " repeated3 off_us=" << off_us << " on_us=" << on_us
                           << " bls_calls=" << values[static_cast<size_t>(dsl::PerfMetric::BLS_VERIFY)].count
                           << " cache_hits=" << values[static_cast<size_t>(dsl::PerfMetric::CACHE_HIT)].count);
        dsl::SetPerfEnabled(false);
    }
}

BOOST_AUTO_TEST_SUITE_END()
