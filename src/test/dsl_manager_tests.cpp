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
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
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

namespace {
CPoSeServiceCommitment MakeCommitment(uint32_t epoch, const uint256& base, size_t size, size_t missedIndex)
{
    CPoSeServiceCommitment c;
    c.nVersion = CPoSeServiceCommitment::OBSERVED_VERSION;
    c.nEpoch = epoch;
    c.epochBlockHash = base;
    c.llmqType = Consensus::LLMQType::LLMQ_TEST;
    c.quorumHash = TaggedHash(epoch, 0, "quorum");
    c.missed.assign(size, false);
    c.observed.assign(size, true);
    if (missedIndex < size) c.missed[missedIndex] = true;
    return c;
}

std::vector<std::byte> Bytes(const CPoSeServiceCommitment& c)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << c;
    return {ss.begin(), ss.end()};
}

uint256 SignedHash(const CPoSeServiceCommitment& c) { return dsl::MakeServiceCommitmentTx(c).msgHash; }
} // namespace

// The transaction a producer builds from a relayed commitment is the one the
// quorum signed: the pool-built candidate goes through the same function, and a
// signature already present on the commitment is not part of the hash.
BOOST_AUTO_TEST_CASE(a_made_commitment_transaction_is_the_signed_one)
{
    auto fx = MakeFixture(12);
    Consensus::Params params;
    const auto built = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_TEST,
                                                     TaggedHash(500, 0, "quorum"), /*reports=*/{}, fx.list, params);
    const auto made = dsl::MakeServiceCommitmentTx(built.commitment);
    BOOST_CHECK(made.msgHash == built.msgHash);
    BOOST_CHECK(CTransaction(made.tx).GetHash() == CTransaction(built.tx).GetHash());

    CPoSeServiceCommitment withSig = built.commitment;
    CBLSSecretKey sk;
    sk.MakeNewKey();
    withSig.quorumSig = sk.Sign(built.msgHash, /*specificLegacyScheme=*/false);
    const auto remade = dsl::MakeServiceCommitmentTx(withSig);
    BOOST_CHECK(remade.msgHash == built.msgHash);
    BOOST_CHECK(!remade.commitment.quorumSig.IsValid());

    // and a different bitfield is a different hash: the signature covers the bits
    CPoSeServiceCommitment flipped = built.commitment;
    flipped.missed[0] = !flipped.missed[0];
    BOOST_CHECK(dsl::MakeServiceCommitmentTx(flipped).msgHash != built.msgHash);
}

// A signed commitment is kept once per (epoch, hash) -- the second copy from
// another peer ends the flood -- and never more than the per-epoch bound.
BOOST_AUTO_TEST_CASE(signed_commitments_are_kept_once_and_bounded)
{
    dsl::CPoSeServiceManager mgr;
    const uint256 base = TaggedHash(10, 0, "epoch");
    mgr.BeginEpoch(10, base);

    const auto c = MakeCommitment(10, base, 16, 3);
    const uint256 h = SignedHash(c);
    BOOST_CHECK(!mgr.HasSignedCommitment(10, h));
    BOOST_CHECK(!mgr.SignedCommitment(10, base, std::nullopt).has_value());
    BOOST_CHECK(mgr.StoreSignedCommitment(c, h));
    BOOST_CHECK(!mgr.StoreSignedCommitment(c, h));
    BOOST_CHECK(mgr.HasSignedCommitment(10, h));

    const auto byHash = mgr.SignedCommitment(10, base, h);
    BOOST_REQUIRE(byHash.has_value());
    BOOST_CHECK(Bytes(*byHash) == Bytes(c));
    const auto any = mgr.SignedCommitment(10, base, std::nullopt);
    BOOST_REQUIRE(any.has_value());
    BOOST_CHECK(Bytes(*any) == Bytes(c));
    BOOST_CHECK(!mgr.SignedCommitment(10, base, TaggedHash(1, 1, "other")).has_value());
    BOOST_CHECK(!mgr.SignedCommitment(9, base, std::nullopt).has_value());

    // the bound: distinct commitments up to kSignedCommitmentsPerEpoch, then no more
    for (size_t i = 1; i < dsl::CPoSeServiceManager::kSignedCommitmentsPerEpoch; ++i) {
        const auto ci = MakeCommitment(10, base, 16, 3 + i);
        BOOST_CHECK(mgr.StoreSignedCommitment(ci, SignedHash(ci)));
    }
    const auto over = MakeCommitment(10, base, 16, 15);
    BOOST_CHECK(!mgr.StoreSignedCommitment(over, SignedHash(over)));
    BOOST_CHECK(!mgr.HasSignedCommitment(10, SignedHash(over)));
    // the first one kept is still the answer without a hash
    const auto stillFirst = mgr.SignedCommitment(10, base, std::nullopt);
    BOOST_REQUIRE(stillFirst.has_value());
    BOOST_CHECK(Bytes(*stillFirst) == Bytes(c));
}

// A producer asks for the commitment of the base its block builds on. One epoch
// can briefly hold two -- a peer delivered the new chain's copy before the tick
// moved the manager -- and the one kept first must not hide the other.
BOOST_AUTO_TEST_CASE(a_signed_commitment_is_looked_up_under_its_own_base)
{
    dsl::CPoSeServiceManager mgr;
    const uint256 oldBase = TaggedHash(10, 0, "epoch");
    const uint256 newBase = TaggedHash(10, 1, "epoch");
    mgr.BeginEpoch(10, oldBase);

    const auto first = MakeCommitment(10, oldBase, 16, 1);
    const auto second = MakeCommitment(10, newBase, 16, 2);
    BOOST_CHECK(mgr.StoreSignedCommitment(first, SignedHash(first)));
    BOOST_CHECK(mgr.StoreSignedCommitment(second, SignedHash(second)));

    const auto onNew = mgr.SignedCommitment(10, newBase, std::nullopt);
    BOOST_REQUIRE(onNew.has_value());
    BOOST_CHECK(Bytes(*onNew) == Bytes(second));
    const auto onOld = mgr.SignedCommitment(10, oldBase, std::nullopt);
    BOOST_REQUIRE(onOld.has_value());
    BOOST_CHECK(Bytes(*onOld) == Bytes(first));
    // looked up by hash, the base still has to match
    BOOST_CHECK(!mgr.SignedCommitment(10, oldBase, SignedHash(second)).has_value());
    BOOST_CHECK(!mgr.SignedCommitment(10, TaggedHash(10, 2, "epoch"), std::nullopt).has_value());
}

// What a member asked to be signed is found by the hash it was signed as, even
// after its pool moved on -- that is the whole point of remembering it.
BOOST_AUTO_TEST_CASE(a_signing_candidate_is_found_by_its_signed_hash)
{
    dsl::CPoSeServiceManager mgr;
    const uint256 base = TaggedHash(10, 0, "epoch");
    mgr.BeginEpoch(10, base);

    const auto c = MakeCommitment(10, base, 16, 2);
    const uint256 h = SignedHash(c);
    BOOST_CHECK(!mgr.SigningCandidate(10, h).has_value());
    mgr.RememberSigningCandidate(c, h);
    mgr.RememberSigningCandidate(c, h);
    const auto found = mgr.SigningCandidate(10, h);
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK(Bytes(*found) == Bytes(c));
    BOOST_CHECK(!mgr.SigningCandidate(10, SignedHash(MakeCommitment(10, base, 16, 4))).has_value());
    // a candidate is not a signed commitment
    BOOST_CHECK(!mgr.HasSignedCommitment(10, h));
}

// Commitments live for their epoch and the one after, and a reorg drops the
// ones it invalidated exactly like the rest of the epoch state.
BOOST_AUTO_TEST_CASE(commitments_follow_the_epoch_window_and_reorgs)
{
    using EpochChange = dsl::CPoSeServiceManager::EpochChange;
    dsl::CPoSeServiceManager mgr;
    const auto base = [](uint32_t e, uint64_t v) { return TaggedHash(e, v, "epoch"); };
    const auto keep = [&](uint32_t e, uint64_t v) {
        const auto c = MakeCommitment(e, base(e, v), 8, e % 8);
        BOOST_CHECK(mgr.StoreSignedCommitment(c, SignedHash(c)));
        return SignedHash(c);
    };

    BOOST_CHECK(mgr.BeginEpoch(10, base(10, 0)) == EpochChange::Entered);
    const uint256 h10 = keep(10, 0);
    BOOST_CHECK(mgr.BeginEpoch(11, base(11, 0)) == EpochChange::Entered);
    BOOST_CHECK(mgr.HasSignedCommitment(10, h10)); // the epoch before stays
    const uint256 h11 = keep(11, 0);
    BOOST_CHECK(mgr.BeginEpoch(12, base(12, 0)) == EpochChange::Entered);
    BOOST_CHECK(!mgr.HasSignedCommitment(10, h10)); // two behind is gone
    BOOST_CHECK(mgr.HasSignedCommitment(11, h11));

    // nothing older than the window is accepted in the first place
    const auto stale = MakeCommitment(10, base(10, 0), 8, 1);
    BOOST_CHECK(!mgr.StoreSignedCommitment(stale, SignedHash(stale)));

    // rebased: epoch 12's base swapped. What was signed over the old base goes;
    // what a peer already delivered for the new base -- the wire can run ahead
    // of the tick -- stays, and so does epoch 11
    const uint256 h12 = keep(12, 0);
    const uint256 h12b = keep(12, 1);
    BOOST_CHECK(mgr.BeginEpoch(12, base(12, 1)) == EpochChange::Rebased);
    BOOST_CHECK(!mgr.HasSignedCommitment(12, h12));
    BOOST_CHECK(mgr.HasSignedCommitment(12, h12b));
    BOOST_CHECK(mgr.HasSignedCommitment(11, h11));

    // rewound back into 11 over a replaced base: everything from 11 up was on
    // the abandoned chain
    BOOST_CHECK(mgr.BeginEpoch(11, base(11, 1)) == EpochChange::Rewound);
    BOOST_CHECK(!mgr.HasSignedCommitment(11, h11));
    BOOST_CHECK(!mgr.HasSignedCommitment(12, h12b));

    // rewound back into 11 above its base: 11's commitment is still the one its
    // boundary needs, while 12 was on the abandoned chain
    const uint256 h11b = keep(11, 1);
    BOOST_CHECK(mgr.BeginEpoch(12, base(12, 2)) == EpochChange::Entered);
    const uint256 h12c = keep(12, 2);
    BOOST_CHECK(mgr.BeginEpoch(11, base(11, 1)) == EpochChange::Rewound);
    BOOST_CHECK(mgr.HasSignedCommitment(11, h11b));
    BOOST_CHECK(!mgr.HasSignedCommitment(12, h12c));
}

// The map stays at two epochs even when BeginEpoch never runs again -- a node
// that is not yet synced keeps receiving commitments while its tick is off.
BOOST_AUTO_TEST_CASE(commitments_stay_bounded_without_the_epoch_tick)
{
    const uint32_t first = 1000, last = 5999;
    const auto commitment = [](uint32_t e, size_t missedIndex) {
        return MakeCommitment(e, TaggedHash(e, 0, "epoch"), 8, missedIndex);
    };

    // relayed commitments only, as a non-member receives them
    {
        dsl::CPoSeServiceManager mgr;
        mgr.BeginEpoch(first, TaggedHash(first, 0, "epoch"));
        std::vector<uint256> hashes;
        for (uint32_t e = first; e <= last; ++e) {
            const auto c = commitment(e, e % 8);
            hashes.push_back(SignedHash(c));
            BOOST_CHECK(mgr.StoreSignedCommitment(c, hashes.back()));
        }
        size_t held = 0;
        for (uint32_t e = first; e <= last; ++e) held += mgr.HasSignedCommitment(e, hashes[e - first]) ? 1 : 0;
        BOOST_CHECK_EQUAL(held, 2U);
        BOOST_CHECK(mgr.HasSignedCommitment(last, hashes.back()));
        BOOST_CHECK(mgr.HasSignedCommitment(last - 1, hashes[last - 1 - first]));

        // a commitment one epoch ahead of the newest does not evict it
        const auto ahead = commitment(last + 1, 0);
        BOOST_CHECK(mgr.StoreSignedCommitment(ahead, SignedHash(ahead)));
        BOOST_CHECK(mgr.HasSignedCommitment(last, hashes.back()));
        BOOST_CHECK(!mgr.HasSignedCommitment(last - 1, hashes[last - 1 - first]));
    }

    // signing candidates only, as a member remembers them
    {
        dsl::CPoSeServiceManager mgr;
        mgr.BeginEpoch(first, TaggedHash(first, 0, "epoch"));
        std::vector<uint256> hashes;
        for (uint32_t e = first; e <= last; ++e) {
            const auto c = commitment(e, (e + 1) % 8);
            hashes.push_back(SignedHash(c));
            mgr.RememberSigningCandidate(c, hashes.back());
        }
        size_t held = 0;
        for (uint32_t e = first; e <= last; ++e) held += mgr.SigningCandidate(e, hashes[e - first]).has_value() ? 1 : 0;
        BOOST_CHECK_EQUAL(held, 2U);
        BOOST_CHECK(mgr.SigningCandidate(last, hashes.back()).has_value());
    }
}

BOOST_AUTO_TEST_SUITE_END()
