// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/pose_service_sentinels.h>
#include <evo/pose_service_store.h>
#include <hash.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_report_store_tests, BasicTestingSetup)

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
} // namespace

BOOST_AUTO_TEST_CASE(accepts_relays_and_dedups)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);

    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);

    auto r = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    // first sight is accepted (and would be relayed)
    BOOST_CHECK(store.AddReport(r, fx.list, fx.epoch, params));
    // a second sight of the same (epoch, target, sentinel) is not
    BOOST_CHECK(!store.AddReport(r, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(store.Size(), 1u);
    BOOST_CHECK(store.HaveReport(500, target, sentinels[0]));

    // a second sentinel for the same target is a distinct report
    auto r2 = SignedReport(500, target, sentinels[1], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[1]]);
    BOOST_CHECK(store.AddReport(r2, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(store.GetReportsForEpoch(500).size(), 2u);
}

BOOST_AUTO_TEST_CASE(rejects_unassigned_and_wrong_key)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(7, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);

    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);

    // a masternode the epoch did not assign to this target cannot report on it
    const uint256 outsider = TaggedHash(29, 0, "protx");
    BOOST_REQUIRE(std::find(sentinels.begin(), sentinels.end(), outsider) == sentinels.end());
    auto bad = SignedReport(500, target, outsider, dsl::ServiceStatus::MISSED, fx.opKeys[outsider]);
    BOOST_CHECK(!store.AddReport(bad, fx.list, fx.epoch, params));

    // an assigned sentinel, but the report is signed with the wrong key
    auto forged = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[outsider]);
    BOOST_CHECK(!store.AddReport(forged, fx.list, fx.epoch, params));

    BOOST_CHECK_EQUAL(store.Size(), 0u);
}

BOOST_AUTO_TEST_CASE(rejects_out_of_window_and_prunes)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(9, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);

    dsl::CServiceReportStore store(/*keepEpochs=*/8);
    store.SetCurrentEpoch(500); // window [493, 500]

    // a report from the future is refused
    auto future = SignedReport(501, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    BOOST_CHECK(!store.AddReport(future, fx.list, fx.epoch, params));
    // one older than the window is refused
    auto ancient = SignedReport(490, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    BOOST_CHECK(!store.AddReport(ancient, fx.list, fx.epoch, params));

    // two in-window reports for different epochs are accepted
    auto r495 = SignedReport(495, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    auto r500 = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    BOOST_CHECK(store.AddReport(r495, fx.list, fx.epoch, params));
    BOOST_CHECK(store.AddReport(r500, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(store.Size(), 2u);

    // advancing the epoch past the window drops both
    store.SetCurrentEpoch(510); // window [503, 510]
    BOOST_CHECK_EQUAL(store.Size(), 0u);
    BOOST_CHECK(store.GetReportsForEpoch(495).empty());
    BOOST_CHECK(store.GetReportsForEpoch(500).empty());
}

BOOST_AUTO_TEST_SUITE_END()
