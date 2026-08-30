// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/pose_service_manager.h>
#include <evo/pose_service_sentinels.h>
#include <hash.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_manager_tests, BasicTestingSetup)

namespace {
uint256 TaggedHash(uint64_t a, uint64_t b, std::string_view tag)
{
    CHashWriter w(SER_GETHASH, 0);
    w << a << b << std::string{tag};
    return w.GetHash();
}

struct Fixture {
    CDeterministicMNList list{uint256(), 1, 0};
    std::map<uint256, CBLSSecretKey> opKeys;
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

// A node in the list that is assigned at least one target this epoch.
uint256 PickSentinelWithTargets(const Fixture& fx, const Consensus::Params& params)
{
    uint256 me;
    fx.list.ForEachMN(false, [&](const auto& dmn) {
        if (!me.IsNull()) return;
        const auto t = dsl::GetProbeTargetsForSentinel(fx.list, dmn.proTxHash, fx.epoch,
                                                       static_cast<size_t>(params.nDSLSentinelCount));
        if (!t.empty()) me = dmn.proTxHash;
    });
    return me;
}
} // namespace

BOOST_AUTO_TEST_CASE(emits_online_for_responders_and_missed_for_silence)
{
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 me = PickSentinelWithTargets(fx, params);
    BOOST_REQUIRE(!me.IsNull());

    dsl::CPoSeServiceManager mgr;
    mgr.BeginEpoch(500, fx.epoch);

    const auto targets = mgr.PendingChallenges(fx.list, me, params);
    BOOST_REQUIRE(!targets.empty());

    // answer the first half; the rest stay silent
    std::set<uint256> responders;
    for (size_t i = 0; i < targets.size() / 2; ++i) {
        mgr.RecordResponse(targets[i]);
        responders.insert(targets[i]);
    }

    const auto reports = mgr.EmitReports(fx.list, me, fx.opKeys[me], params);
    BOOST_CHECK_EQUAL(reports.size(), targets.size());

    const CBLSPublicKey pk = fx.opKeys[me].GetPublicKey();
    for (const auto& r : reports) {
        BOOST_CHECK_EQUAL(r.nEpoch, 500u);
        BOOST_CHECK(r.sentinelProTxHash == me);
        BOOST_CHECK(r.VerifySig(pk));
        const auto want = responders.count(r.targetProTxHash) ? dsl::ServiceStatus::ONLINE
                                                              : dsl::ServiceStatus::MISSED;
        BOOST_CHECK_EQUAL(r.status, static_cast<uint8_t>(want));
    }
}

BOOST_AUTO_TEST_CASE(pending_challenges_drop_responders)
{
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 me = PickSentinelWithTargets(fx, params);
    BOOST_REQUIRE(!me.IsNull());

    dsl::CPoSeServiceManager mgr;
    mgr.BeginEpoch(500, fx.epoch);

    const auto before = mgr.PendingChallenges(fx.list, me, params);
    BOOST_REQUIRE(!before.empty());

    mgr.RecordResponse(before.front());
    const auto after = mgr.PendingChallenges(fx.list, me, params);

    BOOST_CHECK_EQUAL(after.size(), before.size() - 1);
    BOOST_CHECK(std::find(after.begin(), after.end(), before.front()) == after.end());
}

BOOST_AUTO_TEST_CASE(new_epoch_clears_responses)
{
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 me = PickSentinelWithTargets(fx, params);
    BOOST_REQUIRE(!me.IsNull());

    dsl::CPoSeServiceManager mgr;
    mgr.BeginEpoch(500, fx.epoch);
    const auto targets = mgr.PendingChallenges(fx.list, me, params);
    for (const auto& t : targets) mgr.RecordResponse(t);

    // everyone answered -> all ONLINE this epoch
    for (const auto& r : mgr.EmitReports(fx.list, me, fx.opKeys[me], params)) {
        BOOST_CHECK_EQUAL(r.status, static_cast<uint8_t>(dsl::ServiceStatus::ONLINE));
    }

    // the next epoch starts with a clean slate -> all MISSED until answered again
    mgr.BeginEpoch(501, fx.epoch);
    const auto reports = mgr.EmitReports(fx.list, me, fx.opKeys[me], params);
    BOOST_CHECK_EQUAL(reports.size(), targets.size());
    for (const auto& r : reports) {
        BOOST_CHECK_EQUAL(r.nEpoch, 501u);
        BOOST_CHECK_EQUAL(r.status, static_cast<uint8_t>(dsl::ServiceStatus::MISSED));
    }
}

BOOST_AUTO_TEST_SUITE_END()
