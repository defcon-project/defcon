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

namespace dsl {
// Inspect retained allocations without adding a runtime API or timing-based
// assertions. Acceptance alone cannot detect an accidentally unbounded cache.
struct CServiceReportStoreTestAccess {
    static std::map<uint32_t, size_t> CachedTargets(const CServiceReportStore& store)
    {
        LOCK(store.m_mutex);
        std::map<uint32_t, size_t> out;
        for (const auto& [epoch, cache] : store.m_assignCache) out.emplace(epoch, cache.targets.size());
        return out;
    }
};
} // namespace dsl

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

dsl::CPoSeServiceReport SignedReportOn(const uint256& base, uint32_t epoch, const uint256& target,
                                       const uint256& sentinel, dsl::ServiceStatus status,
                                       const CBLSSecretKey& key)
{
    dsl::CPoSeServiceReport r;
    r.nEpoch = epoch;
    r.targetProTxHash = target;
    r.sentinelProTxHash = sentinel;
    r.status = static_cast<uint8_t>(status);
    r.Sign(key, base);
    return r;
}

dsl::CPoSeServiceReport SignedReport(uint32_t epoch, const uint256& target, const uint256& sentinel,
                                     dsl::ServiceStatus status, const CBLSSecretKey& key)
{
    // the fixture's epoch base
    return SignedReportOn(TaggedHash(500, 0, "epoch"), epoch, target, sentinel, status, key);
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

// A report names its target by hash, and the sentinel assignment is derived
// from that hash -- so every 256-bit value has a sentinel set, including values
// that name no masternode. An operator can grind hashes until its own node is
// assigned to one and sign a valid report about a target that does not exist.
// Each accepted report is held until its epoch ages out and flooded to every
// peer, and the store has no bound of its own, so the store is what must refuse
// it.
BOOST_AUTO_TEST_CASE(a_target_that_is_not_a_masternode_is_refused)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 attacker = TaggedHash(11, 0, "protx");

    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);

    // Grind target hashes the way an attacker would, until one assigns the
    // masternode we control. It does not take many: seven sentinels of thirty.
    uint256 ghost;
    bool found = false;
    for (uint64_t i = 0; i < 500 && !found; ++i) {
        const uint256 candidate = TaggedHash(i, 77, "ghost");
        BOOST_REQUIRE(fx.list.GetMN(candidate) == nullptr); // names no masternode
        const auto sentinels = dsl::CalcSentinelsForMN(fx.list, candidate, fx.epoch, 7);
        if (std::find(sentinels.begin(), sentinels.end(), attacker) != sentinels.end()) {
            ghost = candidate;
            found = true;
        }
    }
    BOOST_REQUIRE(found);

    // Correctly signed, by a real masternode this epoch really did assign to
    // that hash -- and still refused, because the hash is nobody.
    auto minted = SignedReport(500, ghost, attacker, dsl::ServiceStatus::MISSED, fx.opKeys[attacker]);
    BOOST_CHECK(!store.AddReport(minted, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(store.Size(), 0u);

    // The same sentinel reporting on a real target it is assigned to still
    // works: the check bounds the store without closing the protocol.
    const uint256 real = TaggedHash(3, 0, "protx");
    const auto real_sentinels = dsl::CalcSentinelsForMN(fx.list, real, fx.epoch, 7);
    auto honest = SignedReport(500, real, real_sentinels[0], dsl::ServiceStatus::MISSED,
                               fx.opKeys[real_sentinels[0]]);
    BOOST_CHECK(store.AddReport(honest, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(store.Size(), 1u);
}

// The store memoises each target's sentinel set for the epoch base it was
// derived against, because deriving it scores the whole masternode list and
// AddReport runs on every report the network floods. A memo that outlived its
// base would be worse than the cost it saves: it would answer questions about
// the current epoch with the previous one's assignment, silently refusing the
// sentinels this epoch actually appointed and admitting the ones it did not.
// Both directions are checked, because a stale memo gets each of them wrong.
BOOST_AUTO_TEST_CASE(assignment_memo_does_not_outlive_its_epoch_base)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(11, 0, "protx");

    const uint256 baseA = fx.epoch;
    const uint256 baseB = TaggedHash(501, 0, "epoch");
    const auto assignedA = dsl::CalcSentinelsForMN(fx.list, target, baseA, 7);
    const auto assignedB = dsl::CalcSentinelsForMN(fx.list, target, baseB, 7);

    const auto only_in = [](const std::vector<uint256>& a, const std::vector<uint256>& b) {
        std::vector<uint256> out;
        for (const auto& h : a) {
            if (std::find(b.begin(), b.end(), h) == b.end()) out.push_back(h);
        }
        return out;
    };
    const auto onlyA = only_in(assignedA, assignedB);
    const auto onlyB = only_in(assignedB, assignedA);
    // If a base change moved nobody, this test proves nothing -- say so rather
    // than passing.
    BOOST_REQUIRE(!onlyA.empty());
    BOOST_REQUIRE(!onlyB.empty());

    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);

    // epoch 500 on base A: accepted, and the memo for this target is now filled
    auto seed = SignedReportOn(baseA, 500, target, assignedA[0], dsl::ServiceStatus::ONLINE,
                               fx.opKeys[assignedA[0]]);
    BOOST_CHECK(store.AddReport(seed, fx.list, baseA, params));

    // epoch 501 on base B, same target: the memo must have been discarded
    store.SetCurrentEpoch(501);

    // a sentinel base A appointed and base B did not is refused -- a stale memo
    // would have accepted it
    auto stale = SignedReportOn(baseB, 501, target, onlyA[0], dsl::ServiceStatus::MISSED,
                                fx.opKeys[onlyA[0]]);
    BOOST_CHECK(!store.AddReport(stale, fx.list, baseB, params));

    // and a sentinel base B appointed is accepted -- a stale memo would have
    // refused it, which is the quieter half of the same bug
    auto fresh = SignedReportOn(baseB, 501, target, onlyB[0], dsl::ServiceStatus::MISSED,
                                fx.opKeys[onlyB[0]]);
    BOOST_CHECK(store.AddReport(fresh, fx.list, baseB, params));

    BOOST_CHECK_EQUAL(store.GetReportsForEpoch(501).size(), 1u);
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

BOOST_AUTO_TEST_CASE(assignment_cache_retains_only_the_report_window)
{
    auto fx = MakeFixture(30);
    const Consensus::Params params{};
    dsl::CServiceReportStore store(8);
    store.SetCurrentEpoch(500);
    const auto add = [&](uint32_t epoch, size_t target_id, size_t sender) {
        const auto base = TaggedHash(epoch, 0, "epoch");
        const auto target = TaggedHash(target_id, 0, "protx");
        const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, base, params.nDSLSentinelCount);
        return store.AddReport(SignedReportOn(base, epoch, target, sentinels.at(sender),
            dsl::ServiceStatus::ONLINE, fx.opKeys.at(sentinels.at(sender))), fx.list, base, params);
    };
    // Alternate epochs and targets, then send a second distinct report for
    // every pair. Earlier assignments must survive these interleaved reports.
    std::map<uint32_t, size_t> expected;
    for (size_t sender = 0; sender < 2; ++sender) {
        for (size_t target = 0; target < 2; ++target) {
            for (uint32_t epoch = 493; epoch <= 500; ++epoch) {
                BOOST_REQUIRE(add(epoch, target, sender));
                expected[epoch] = target + 1;
            }
        }
        BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store) == expected);
    }
    BOOST_CHECK_EQUAL(store.Size(), 32U);

    store.SetCurrentEpoch(501);
    expected.erase(493);
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store) == expected);
    BOOST_CHECK_EQUAL(store.Size(), 28U);
    BOOST_CHECK(!add(493, 0, 2));
    BOOST_CHECK(!add(502, 0, 2));
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store) == expected);
    BOOST_REQUIRE(add(501, 0, 0));
    expected.emplace(501, 1);
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store) == expected);

    store.DropEpoch(499);
    expected.erase(499);
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store) == expected);
    BOOST_CHECK(store.GetReportsForEpoch(499).empty());
    BOOST_REQUIRE(add(499, 0, 0));
    expected.emplace(499, 1);
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store) == expected);

    store.SetCurrentEpoch(498); // Rewind: invalidate assignments and refill lazily.
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store).empty());
    BOOST_REQUIRE(add(498, 0, 2));
    BOOST_CHECK_EQUAL(dsl::CServiceReportStoreTestAccess::CachedTargets(store).size(), 1U);
    store.SetCurrentEpoch(510);
    BOOST_CHECK(dsl::CServiceReportStoreTestAccess::CachedTargets(store).empty());
    BOOST_CHECK_EQUAL(store.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(assignment_cache_replaces_a_base_or_count_within_one_epoch)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const auto target = TaggedHash(11, 0, "protx");
    const auto baseA = fx.epoch;
    const auto baseB = TaggedHash(501, 0, "epoch");
    const auto assignedA = dsl::CalcSentinelsForMN(fx.list, target, baseA, 7);
    const auto assignedB = dsl::CalcSentinelsForMN(fx.list, target, baseB, 7);
    const auto onlyA = std::find_if(assignedA.begin(), assignedA.end(), [&](const auto& hash) {
        return std::find(assignedB.begin(), assignedB.end(), hash) == assignedB.end();
    });
    const auto onlyB = std::find_if(assignedB.begin(), assignedB.end(), [&](const auto& hash) {
        return std::find(assignedA.begin(), assignedA.end(), hash) == assignedA.end();
    });
    BOOST_REQUIRE(onlyA != assignedA.end());
    BOOST_REQUIRE(onlyB != assignedB.end());
    const auto seed = *std::find_if(assignedA.begin(), assignedA.end(), [&](const auto& h) { return h != *onlyA; });
    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);
    const auto add = [&](const auto& base, const auto& sender) {
        return store.AddReport(SignedReportOn(base, 500, target, sender,
            dsl::ServiceStatus::ONLINE, fx.opKeys.at(sender)), fx.list, base, params);
    };
    BOOST_REQUIRE(add(baseA, seed));
    BOOST_CHECK(!add(baseB, *onlyA)); // New base must not use the old assignment.
    BOOST_REQUIRE(add(baseB, *onlyB));
    BOOST_REQUIRE(add(baseA, *onlyA)); // Returning to A also replaces B.
    BOOST_CHECK_EQUAL(dsl::CServiceReportStoreTestAccess::CachedTargets(store).size(), 1U);

    store.DropEpoch(500);
    BOOST_REQUIRE(add(baseA, assignedA[0]));
    params.nDSLSentinelCount = 1;
    BOOST_CHECK(!add(baseA, assignedA[1]));
    params.nDSLSentinelCount = 7;
    BOOST_REQUIRE(add(baseA, assignedA[1]));
    const auto cached = dsl::CServiceReportStoreTestAccess::CachedTargets(store);
    BOOST_REQUIRE_EQUAL(cached.size(), 1U);
    BOOST_CHECK_EQUAL(cached.at(500), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
