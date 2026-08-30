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
        BOOST_CHECK(r.VerifySig(pk, fx.epoch));
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

BOOST_AUTO_TEST_CASE(liveness_announcements_flood_once_and_bind_identity)
{
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 me = PickSentinelWithTargets(fx, params);
    BOOST_REQUIRE(!me.IsNull());

    dsl::CPoSeServiceManager mgr;
    mgr.BeginEpoch(500, fx.epoch);
    const auto targets = mgr.PendingChallenges(fx.list, me, params);
    BOOST_REQUIRE(!targets.empty());
    const uint256 target = targets.front();

    // the target's own announcement builds against its manager view...
    dsl::CPoSeServiceManager targetMgr;
    targetMgr.BeginEpoch(500, fx.epoch);
    const auto ann = targetMgr.AnnounceLiveness(target, fx.opKeys[target]);
    BOOST_CHECK_EQUAL(ann.nEpoch, 500u);
    BOOST_CHECK(ann.proTxHash == target);

    // ...and is accepted here on first sight (relay) and refused as a duplicate
    BOOST_CHECK(mgr.ProcessResponse(ann, fx.list, fx.epoch));
    BOOST_CHECK(!mgr.ProcessResponse(ann, fx.list, fx.epoch));
    const auto after = mgr.PendingChallenges(fx.list, me, params);
    BOOST_CHECK(std::find(after.begin(), after.end(), target) == after.end());

    // a replay into another epoch fails: that epoch's base hash differs, and the
    // signature was bound to this one
    dsl::CPoSeServiceResponse replayed = ann;
    replayed.nEpoch = 499;
    BOOST_CHECK(!mgr.ProcessResponse(replayed, fx.list, TaggedHash(499, 0, "epoch")));

    // claiming another node's identity fails on its operator key
    dsl::CPoSeServiceResponse stolen;
    stolen.nEpoch = 500;
    stolen.proTxHash = me; // claims to be `me`, but signs with the target's key
    stolen.sig = dsl::SignChallengeResponse(fx.opKeys[target], fx.epoch, me);
    BOOST_CHECK(!mgr.ProcessResponse(stolen, fx.list, fx.epoch));

    // an announcement from a node not on the list is refused
    dsl::CPoSeServiceResponse ghost;
    ghost.nEpoch = 500;
    ghost.proTxHash = TaggedHash(999, 0, "protx");
    CBLSSecretKey ghostKey;
    ghostKey.MakeNewKey();
    ghost.sig = dsl::SignChallengeResponse(ghostKey, fx.epoch, ghost.proTxHash);
    BOOST_CHECK(!mgr.ProcessResponse(ghost, fx.list, fx.epoch));
}

// An announcement that outruns the local epoch tick -- seen on a live regtest
// network, where the wire beat the validation-interface queue by an epoch --
// must be accepted into its own epoch's set, not dropped, because the flood
// forwards each copy only once and a drop is permanent.
BOOST_AUTO_TEST_CASE(announcement_ahead_of_the_local_tick_is_kept)
{
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 me = PickSentinelWithTargets(fx, params);
    BOOST_REQUIRE(!me.IsNull());

    dsl::CPoSeServiceManager mgr;
    mgr.BeginEpoch(500, fx.epoch);

    // a target announces for epoch 501 before this node's tick got there
    const uint256 epoch501 = TaggedHash(501, 0, "epoch");
    dsl::CPoSeServiceManager targetMgr;
    targetMgr.BeginEpoch(501, epoch501);
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto ann = targetMgr.AnnounceLiveness(target, fx.opKeys[target]);
    BOOST_CHECK_EQUAL(ann.nEpoch, 501u);

    // accepted while we still sit in epoch 500 (the caller supplies 501's hash)
    BOOST_CHECK(mgr.ProcessResponse(ann, fx.list, epoch501));
    // it does not pollute the current epoch...
    BOOST_CHECK(!mgr.HasResponded(target));
    // ...and once our tick catches up, it is already there
    mgr.BeginEpoch(501, epoch501);
    BOOST_CHECK(mgr.HasResponded(target));
    BOOST_CHECK_EQUAL(mgr.RespondedCount(), 1u);
}

BOOST_AUTO_TEST_CASE(process_report_pools_and_refuses_unknown_epoch)
{
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch,
                                                   static_cast<size_t>(params.nDSLSentinelCount));
    BOOST_REQUIRE(!sentinels.empty());
    const uint256 sentinel = sentinels.front();

    dsl::CPoSeServiceManager mgr;
    mgr.BeginEpoch(500, fx.epoch);

    dsl::CPoSeServiceReport rep;
    rep.nEpoch = 500;
    rep.targetProTxHash = target;
    rep.sentinelProTxHash = sentinel;
    rep.status = static_cast<uint8_t>(dsl::ServiceStatus::MISSED);
    rep.Sign(fx.opKeys[sentinel], fx.epoch);

    // a valid peer report pools once (and would be relayed); a duplicate does not
    BOOST_CHECK(mgr.ProcessReport(rep, fx.list, fx.epoch, params));
    BOOST_CHECK(!mgr.ProcessReport(rep, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(500).size(), 1u);

    // a report for an epoch far outside the retained window is refused
    dsl::CPoSeServiceReport ancient = rep;
    ancient.nEpoch = 400;
    ancient.Sign(fx.opKeys[sentinel], fx.epoch);
    BOOST_CHECK(!mgr.ProcessReport(ancient, fx.list, fx.epoch, params));
}

// A reorg that swaps an epoch's base block must discard the state gathered
// under the old base -- otherwise a responder counts as seen and its fresh
// announcement is refused as a duplicate -- and report the rebase so the net
// layer re-runs its once-per-epoch actions.
BOOST_AUTO_TEST_CASE(reorg_rebasing_an_epoch_clears_its_state)
{
    using EpochChange = dsl::CPoSeServiceManager::EpochChange;
    auto fx = MakeFixture(40);
    Consensus::Params params;
    const uint256 me = PickSentinelWithTargets(fx, params);
    BOOST_REQUIRE(!me.IsNull());

    dsl::CPoSeServiceManager mgr;
    const uint256 baseA = fx.epoch;
    BOOST_CHECK(mgr.BeginEpoch(500, baseA) == EpochChange::Entered);

    // a target announces under base A, and a report for it pools
    const auto targets = mgr.PendingChallenges(fx.list, me, params);
    BOOST_REQUIRE(!targets.empty());
    const uint256 target = targets.front();
    dsl::CPoSeServiceResponse ann;
    ann.nEpoch = 500;
    ann.proTxHash = target;
    ann.sig = dsl::SignChallengeResponse(fx.opKeys[target], baseA, target);
    BOOST_CHECK(mgr.ProcessResponse(ann, fx.list, baseA));
    BOOST_CHECK(mgr.HasResponded(target));
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, baseA,
                                                   static_cast<size_t>(params.nDSLSentinelCount));
    dsl::CPoSeServiceReport rep;
    rep.nEpoch = 500;
    rep.targetProTxHash = target;
    rep.sentinelProTxHash = sentinels.front();
    rep.status = static_cast<uint8_t>(dsl::ServiceStatus::MISSED);
    rep.Sign(fx.opKeys[sentinels.front()], baseA);
    BOOST_CHECK(mgr.ProcessReport(rep, fx.list, baseA, params));
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(500).size(), 1u);

    // a same-epoch reorg to a new base clears both, and reports Rebased
    const uint256 baseB = TaggedHash(500, 1, "epoch");
    BOOST_CHECK(mgr.BeginEpoch(500, baseB) == EpochChange::Rebased);
    BOOST_CHECK(!mgr.HasResponded(target));
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(500).size(), 0u);

    // the target can announce fresh under base B
    dsl::CPoSeServiceResponse annB;
    annB.nEpoch = 500;
    annB.proTxHash = target;
    annB.sig = dsl::SignChallengeResponse(fx.opKeys[target], baseB, target);
    BOOST_CHECK(mgr.ProcessResponse(annB, fx.list, baseB));

    // and calling again for the same (epoch, base) is a no-op
    BOOST_CHECK(mgr.BeginEpoch(500, baseB) == EpochChange::None);
}

// A reorg that moves the tip back across an epoch boundary must drop every
// epoch it rewound through -- the one it lands on (whose base block changed too)
// and every higher one observed on the abandoned chain -- so no stale response
// or report survives into the re-observation, and Rewound is reported.
BOOST_AUTO_TEST_CASE(reorg_rewinding_past_a_boundary_drops_stale_epochs)
{
    using EpochChange = dsl::CPoSeServiceManager::EpochChange;
    auto fx = MakeFixture(40);
    Consensus::Params params;

    const uint256 target = TaggedHash(3, 0, "protx");
    const auto seed = [&](dsl::CPoSeServiceManager& m, uint32_t epoch, const uint256& base) {
        dsl::CPoSeServiceResponse ann;
        ann.nEpoch = epoch;
        ann.proTxHash = target;
        ann.sig = dsl::SignChallengeResponse(fx.opKeys[target], base, target);
        BOOST_CHECK(m.ProcessResponse(ann, fx.list, base));
        const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, base,
                                                       static_cast<size_t>(params.nDSLSentinelCount));
        dsl::CPoSeServiceReport rep;
        rep.nEpoch = epoch;
        rep.targetProTxHash = target;
        rep.sentinelProTxHash = sentinels.front();
        rep.status = static_cast<uint8_t>(dsl::ServiceStatus::MISSED);
        rep.Sign(fx.opKeys[sentinels.front()], base);
        BOOST_CHECK(m.ProcessReport(rep, fx.list, base, params));
    };

    dsl::CPoSeServiceManager mgr;
    const uint256 base500 = fx.epoch; // TaggedHash(500, 0, "epoch")
    const uint256 base501 = TaggedHash(501, 0, "epoch");

    BOOST_CHECK(mgr.BeginEpoch(500, base500) == EpochChange::Entered);
    seed(mgr, 500, base500);
    BOOST_CHECK(mgr.BeginEpoch(501, base501) == EpochChange::Entered);
    seed(mgr, 501, base501);
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(500).size(), 1u);
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(501).size(), 1u);

    // the tip is reorged back into epoch 500 on a new base: 500 (its base
    // changed) and 501 (gone with the abandoned chain) are both dropped
    const uint256 base500b = TaggedHash(500, 2, "epoch");
    BOOST_CHECK(mgr.BeginEpoch(500, base500b) == EpochChange::Rewound);
    BOOST_CHECK(!mgr.HasResponded(target));
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(500).size(), 0u);
    BOOST_CHECK_EQUAL(mgr.Store().GetReportsForEpoch(501).size(), 0u);

    // the target announces fresh under the new base, unblocked by the stale seen-set
    dsl::CPoSeServiceResponse annB;
    annB.nEpoch = 500;
    annB.proTxHash = target;
    annB.sig = dsl::SignChallengeResponse(fx.opKeys[target], base500b, target);
    BOOST_CHECK(mgr.ProcessResponse(annB, fx.list, base500b));
    BOOST_CHECK(mgr.HasResponded(target));
}

BOOST_AUTO_TEST_SUITE_END()
