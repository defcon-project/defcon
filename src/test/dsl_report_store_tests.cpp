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
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
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

BOOST_AUTO_TEST_CASE(cache_hits_do_not_bypass_admission_and_parallel_dedup)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    const auto pk = fx.opKeys.at(sentinels[0]).GetPublicKey();
    auto good = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::ONLINE, fx.opKeys.at(sentinels[0]));
    BOOST_REQUIRE(good.VerifySig(pk, fx.epoch));
    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
        if (store.AddReport(good, fx.list, fx.epoch, params)) ++accepted;
    });
    for (auto& worker : workers) worker.join();
    BOOST_CHECK_EQUAL(accepted.load(), 1);
    BOOST_CHECK_EQUAL(store.Size(), 1u);
    store.DropEpoch(500);
    BOOST_CHECK(!store.AddReport(good, fx.list, TaggedHash(501, 0, "epoch"), params));
    BOOST_CHECK(store.AddReport(good, fx.list, fx.epoch, params));
    store.SetCurrentEpoch(600);
    BOOST_CHECK(good.VerifySig(pk, fx.epoch)); // still a true crypto fact
    BOOST_CHECK(!store.AddReport(good, fx.list, fx.epoch, params)); // aged out

    store.SetCurrentEpoch(500);
    auto unknown = good;
    unknown.targetProTxHash = TaggedHash(123456, 0, "unknown");
    unknown.Sign(fx.opKeys.at(sentinels[0]), fx.epoch);
    BOOST_REQUIRE(unknown.VerifySig(pk, fx.epoch));
    BOOST_CHECK(!store.AddReport(unknown, fx.list, fx.epoch, params));
    const auto outsider = std::find_if(fx.opKeys.begin(), fx.opKeys.end(), [&](const auto& entry) {
        return std::find(sentinels.begin(), sentinels.end(), entry.first) == sentinels.end();
    });
    BOOST_REQUIRE(outsider != fx.opKeys.end());
    auto unassigned = good;
    unassigned.sentinelProTxHash = outsider->first;
    unassigned.Sign(outsider->second, fx.epoch);
    BOOST_REQUIRE(unassigned.VerifySig(outsider->second.GetPublicKey(), fx.epoch));
    BOOST_CHECK(!store.AddReport(unassigned, fx.list, fx.epoch, params));
    BOOST_CHECK_EQUAL(store.Size(), 0u);
}

namespace {
std::vector<unsigned char> WireOf(const dsl::CPoSeServiceReport& report)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << report;
    return {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()};
}

// The view refers into `bytes`, which has to outlive it.
dsl::ReportWire PeekOf(const std::vector<unsigned char>& bytes)
{
    const auto wire = dsl::PeekReportWire(MakeUCharSpan(bytes));
    BOOST_REQUIRE(wire.has_value());
    return *wire;
}
} // namespace

// Reports are flooded without an inventory step, and nearly every arrival is a
// copy of one already held. The store can say that a payload is such a copy from
// its bytes alone, which lets the caller drop it without decoding the signature.
// It may say so only when the payload is the held report in every byte.
BOOST_AUTO_TEST_CASE(a_held_report_is_recognised_from_its_bytes_and_only_by_all_of_them)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);

    const auto report = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    const auto bytes = WireOf(report);
    BOOST_REQUIRE_EQUAL(bytes.size(), dsl::CPoSeServiceReport::WIRE_SIZE);
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(bytes))); // nothing held yet
    BOOST_REQUIRE(store.AddReport(report, fx.list, fx.epoch, params));
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));

    // one bit of any of the 165 bytes, and it is not the same report
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto other = bytes;
        other[i] ^= 0x01;
        BOOST_CHECK_MESSAGE(!store.HaveIdenticalReport(PeekOf(other)),
                            "a flipped bit in byte " << i << " was taken for the held report");
    }
    // the same key and signature under another status, or the same key and
    // status under another valid signature: neither is the held report
    auto flipped = report;
    flipped.status = static_cast<uint8_t>(dsl::ServiceStatus::ONLINE);
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(WireOf(flipped))));
    auto resigned = SignedReport(500, target, sentinels[1], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[1]]);
    resigned.sentinelProTxHash = sentinels[0];
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(WireOf(resigned))));
    // a report that is not held says nothing
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(WireOf(SignedReport(500, target, sentinels[1],
        dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[1]])))));

    // asking changes nothing
    BOOST_CHECK_EQUAL(store.Size(), 1u);
    BOOST_CHECK(store.HaveReport(500, target, sentinels[0]));
    BOOST_CHECK(!store.HaveReport(500, target, sentinels[1]));
}

// The question is asked only where AddReport would get as far as its duplicate
// test: a held report outside the epoch window is none of its business, and
// what the store has dropped is not held.
BOOST_AUTO_TEST_CASE(a_held_report_is_recognised_only_inside_the_epoch_window)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    dsl::CServiceReportStore store(/*keepEpochs=*/8);
    store.SetCurrentEpoch(500);
    const auto report = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    const auto bytes = WireOf(report);
    BOOST_REQUIRE(store.AddReport(report, fx.list, fx.epoch, params));
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));

    // the store behind the report's epoch (a rewind): held, but AddReport would
    // call it a report from the future, and so must the caller's ordinary path
    store.SetCurrentEpoch(499);
    BOOST_CHECK(store.HaveReport(500, target, sentinels[0]));
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(bytes)));
    BOOST_CHECK(!store.AddReport(report, fx.list, fx.epoch, params)); // refused as from the future

    store.SetCurrentEpoch(500);
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));
    store.SetCurrentEpoch(507); // the window is [500, 507]: the last epoch the report is kept for
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));
    store.SetCurrentEpoch(508); // [501, 508]: aged out, and dropped
    BOOST_CHECK_EQUAL(store.Size(), 0u);
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(bytes)));

    // dropping an epoch (a reorg of its base) forgets what was held in it
    store.SetCurrentEpoch(500);
    BOOST_REQUIRE(store.AddReport(report, fx.list, fx.epoch, params));
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));
    store.DropEpoch(500);
    BOOST_CHECK(!store.HaveIdenticalReport(PeekOf(bytes)));
    BOOST_CHECK(store.AddReport(report, fx.list, fx.epoch, params)); // new again
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));
}

// The check runs on the message thread beside changes made on the validation
// thread: nothing it reads may be torn, and nothing it does may disturb them.
BOOST_AUTO_TEST_CASE(recognising_a_held_report_is_safe_beside_changes_to_the_store)
{
    auto fx = MakeFixture(30);
    Consensus::Params params;
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    dsl::CServiceReportStore store;
    store.SetCurrentEpoch(500);
    const auto report = SignedReport(500, target, sentinels[0], dsl::ServiceStatus::MISSED, fx.opKeys[sentinels[0]]);
    const auto bytes = WireOf(report);
    BOOST_REQUIRE(store.AddReport(report, fx.list, fx.epoch, params));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> asked{0};
    std::vector<std::thread> askers;
    for (int i = 0; i < 4; ++i) askers.emplace_back([&] {
        const auto wire = dsl::PeekReportWire(MakeUCharSpan(bytes));
        while (!stop.load()) {
            (void)store.HaveIdenticalReport(*wire);
            ++asked;
        }
    });
    bool all_added{true};
    for (int round = 0; round < 100; ++round) {
        store.DropEpoch(500);
        all_added = store.AddReport(report, fx.list, fx.epoch, params) && all_added;
        store.SetCurrentEpoch(499); // the window moves under the askers, and back
        store.SetCurrentEpoch(500);
    }
    stop = true;
    for (auto& asker : askers) asker.join();
    BOOST_CHECK(all_added);
    BOOST_CHECK_GT(asked.load(), 0u);
    BOOST_CHECK(store.HaveIdenticalReport(PeekOf(bytes)));
    BOOST_CHECK_EQUAL(store.Size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
