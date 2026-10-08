// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Service-commitment format version 2: the observed bitfield.
//
// Version 1 carries one bitfield, `missed`, so a masternode the epoch's
// sentinels could not judge -- too few reports, or a split -- is
// indistinguishable from one they saw online, and applying the commitment
// heals it. Version 2 adds `observed`; an unobserved bit changes nothing.
// These cases pin the three pieces: which version a height requires, what the
// aggregator writes into both bitfields, and what applying each version does.

#include <bls/bls.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/pose_service.h>
#include <evo/pose_service_sentinels.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_commitment_v2_tests, BasicTestingSetup)

namespace {
uint256 TaggedHash(uint64_t a, uint64_t b, std::string_view tag)
{
    CHashWriter w(SER_GETHASH, 0);
    w << a << b << std::string{tag};
    return w.GetHash();
}

// The aggregation fixture: a confirmed list whose members hold operator keys,
// so sentinel reports can be signed and verified the way consensus does.
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

dsl::CPoSeServiceReport SignedReport(uint32_t epoch, const uint256& target, const uint256& sentinel,
                                     dsl::ServiceStatus status, const CBLSSecretKey& key)
{
    dsl::CPoSeServiceReport r;
    r.nEpoch = epoch;
    r.targetProTxHash = target;
    r.sentinelProTxHash = sentinel;
    r.status = static_cast<uint8_t>(status);
    r.Sign(key, TaggedHash(500, 0, "epoch"));
    return r;
}

size_t CanonicalIndex(const CDeterministicMNList& list, const uint256& protx)
{
    std::vector<uint256> order;
    list.ForEachMN(false, [&](const auto& dmn) { order.push_back(dmn.proTxHash); });
    std::sort(order.begin(), order.end());
    return static_cast<size_t>(std::find(order.begin(), order.end(), protx) - order.begin());
}

// `online` ONLINE reports and `missed` MISSED reports for `target`, each from a
// distinct assigned sentinel.
std::vector<dsl::CPoSeServiceReport> Reports(const Fixture& fx, const uint256& target, size_t online, size_t missed)
{
    const auto sentinels = dsl::CalcSentinelsForMN(fx.list, target, fx.epoch, 7);
    BOOST_REQUIRE(online + missed <= sentinels.size());
    std::vector<dsl::CPoSeServiceReport> out;
    size_t k = 0;
    for (size_t i = 0; i < online; ++i, ++k) {
        out.push_back(SignedReport(500, target, sentinels[k], dsl::ServiceStatus::ONLINE, fx.opKeys.at(sentinels[k])));
    }
    for (size_t i = 0; i < missed; ++i, ++k) {
        out.push_back(SignedReport(500, target, sentinels[k], dsl::ServiceStatus::MISSED, fx.opKeys.at(sentinels[k])));
    }
    return out;
}

// Aggregation params: 7 sentinels, 5 agree (the defaults), with the format
// flip at `v2_height`. The fixture's epoch 500 closes at height 501 * 24.
Consensus::Params AggParams(int v2_height)
{
    Consensus::Params p;
    p.nDSLCommitmentV2Height = v2_height;
    return p;
}

// The apply fixture: a plain list, commitments built by hand.
std::pair<CDeterministicMNList, std::vector<uint256>> MakeList(size_t n)
{
    CDeterministicMNList list(uint256(), /*height=*/100, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        uint256 protx;
        std::memset(protx.begin(), static_cast<int>(i + 1), protx.size());
        dmn->proTxHash = protx;
        dmn->collateralOutpoint = COutPoint(protx, 0);
        auto st = std::make_shared<CDeterministicMNState>();
        st->nRegisteredHeight = 1;
        CKeyID owner;
        std::memcpy(owner.begin(), protx.begin(), owner.size());
        st->keyIDOwner = owner;
        dmn->pdmnState = st;
        list.AddMN(dmn);
    }
    std::vector<uint256> order;
    list.ForEachMN(false, [&](const auto& dmn) { order.push_back(dmn.proTxHash); });
    std::sort(order.begin(), order.end());
    return {std::move(list), std::move(order)};
}

Consensus::Params ApplyParams()
{
    Consensus::Params p;
    p.nDSLSuspendEpochs = 4;
    p.nDSLBanEpochs = 5;
    p.nDSLMassOutagePct = 15;
    p.nDSLEnforcementHeight = 0; // enforcing
    return p;
}

// A version-2 commitment over n masternodes: index 0 missed (and observed),
// every other index observed online -- unless `zero_unobserved`, in which
// case index 0 carries no verdict at all.
CPoSeServiceCommitment V2(uint32_t epoch, size_t n, bool zero_missed, bool zero_unobserved)
{
    CPoSeServiceCommitment c;
    c.nVersion = CPoSeServiceCommitment::OBSERVED_VERSION;
    c.nEpoch = epoch;
    c.missed.assign(n, false);
    c.observed.assign(n, true);
    if (zero_unobserved) c.observed[0] = false;
    if (zero_missed) c.missed[0] = true;
    return c;
}
} // namespace

// The flip height decides the format, one way, with exactly one format valid
// at any height. Unset (0) means version 2 from the first commitment.
BOOST_AUTO_TEST_CASE(required_version_follows_the_flip_height)
{
    Consensus::Params p;
    BOOST_CHECK_EQUAL(p.nDSLCommitmentV2Height, 0);
    BOOST_CHECK_EQUAL(RequiredServiceCommitmentVersion(p, 0), CPoSeServiceCommitment::OBSERVED_VERSION);
    BOOST_CHECK_EQUAL(RequiredServiceCommitmentVersion(p, 5496), CPoSeServiceCommitment::OBSERVED_VERSION);

    p.nDSLCommitmentV2Height = 240;
    BOOST_CHECK_EQUAL(RequiredServiceCommitmentVersion(p, 239), CPoSeServiceCommitment::LEGACY_VERSION);
    BOOST_CHECK_EQUAL(RequiredServiceCommitmentVersion(p, 240), CPoSeServiceCommitment::OBSERVED_VERSION);
    BOOST_CHECK_EQUAL(RequiredServiceCommitmentVersion(p, 100000), CPoSeServiceCommitment::OBSERVED_VERSION);

    p.nDSLCommitmentV2Height = std::numeric_limits<int>::max();
    BOOST_CHECK_EQUAL(RequiredServiceCommitmentVersion(p, 100000), CPoSeServiceCommitment::LEGACY_VERSION);
}

// Version 2 has three states per index. MISSED needs five agreeing sentinels,
// exactly as before; ONLINE now needs five too; anything short of either -- too
// few reports, or a split -- is unobserved.
BOOST_AUTO_TEST_CASE(aggregation_reaches_three_states)
{
    auto fx = MakeFixture(30);
    const auto params = AggParams(/*v2_height=*/0);
    const uint256 target = TaggedHash(3, 0, "protx");
    const size_t idx = CanonicalIndex(fx.list, target);

    const auto build = [&](size_t online, size_t missed) {
        return dsl::BuildServiceCommitment(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60, uint256::ONE,
                                           Reports(fx, target, online, missed), fx.list, params);
    };

    // five MISSED: a verdict, and it is missed
    auto c = build(0, 5);
    BOOST_CHECK_EQUAL(c.nVersion, CPoSeServiceCommitment::OBSERVED_VERSION);
    BOOST_CHECK_EQUAL(c.observed.size(), c.missed.size());
    BOOST_CHECK(c.observed[idx]);
    BOOST_CHECK(c.missed[idx]);
    BOOST_CHECK_EQUAL(c.CountMissed(), 1);

    // five ONLINE: a verdict, and it is online
    c = build(5, 0);
    BOOST_CHECK(c.observed[idx]);
    BOOST_CHECK(!c.missed[idx]);

    // four ONLINE and three MISSED: seven valid reports, no verdict either way
    c = build(4, 3);
    BOOST_CHECK(!c.observed[idx]);
    BOOST_CHECK(!c.missed[idx]);

    // two ONLINE: too few to say anything
    c = build(2, 0);
    BOOST_CHECK(!c.observed[idx]);
    BOOST_CHECK(!c.missed[idx]);

    // four MISSED: below the threshold on that side, and not online either
    c = build(0, 4);
    BOOST_CHECK(!c.observed[idx]);
    BOOST_CHECK(!c.missed[idx]);

    // nothing at all: unobserved, and so is every other masternode, which
    // nobody reported on
    c = build(0, 0);
    BOOST_CHECK(!c.observed[idx]);
    BOOST_CHECK_EQUAL(c.CountUnobserved(), 30);
    BOOST_CHECK_EQUAL(c.CountMissed(), 0);

    // whatever it built, the bitfield invariants hold
    TxValidationState state;
    BOOST_CHECK(CheckServiceCommitmentBitfields(build(0, 5), state));
    BOOST_CHECK(CheckServiceCommitmentBitfields(build(4, 3), state));
    BOOST_CHECK(CheckServiceCommitmentBitfields(build(0, 0), state));
}

// Below the flip the aggregator writes version 1: no observed field, and the
// missed bits exactly as version 2 would set them -- the missed rule did not
// move, only the ability to say "no verdict" was added.
BOOST_AUTO_TEST_CASE(legacy_format_below_the_flip)
{
    auto fx = MakeFixture(30);
    const uint256 target = TaggedHash(3, 0, "protx");
    const size_t idx = CanonicalIndex(fx.list, target);
    const auto legacy = AggParams(std::numeric_limits<int>::max());
    const auto current = AggParams(0);

    const std::vector<std::pair<size_t, size_t>> cases{{0, 5}, {5, 0}, {4, 3}, {0, 0}};
    for (const auto& [online, missed] : cases) {
        const auto reports = Reports(fx, target, online, missed);
        const auto v1 = dsl::BuildServiceCommitment(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60, uint256::ONE,
                                                    reports, fx.list, legacy);
        const auto v2 = dsl::BuildServiceCommitment(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60, uint256::ONE,
                                                    reports, fx.list, current);
        BOOST_CHECK_EQUAL(v1.nVersion, CPoSeServiceCommitment::LEGACY_VERSION);
        BOOST_CHECK(v1.observed.empty());
        BOOST_CHECK(v1.missed == v2.missed);
        BOOST_CHECK(v1.missed[idx] == (missed >= 5));
        // and version 1 observes everyone, by definition of the format
        BOOST_CHECK(v1.IsObserved(idx));
        BOOST_CHECK_EQUAL(v1.CountUnobserved(), 0);
    }

    // the two formats serialize differently, so they sign different messages:
    // a quorum on the old format and one on the new cannot agree on a hash
    const auto reports = Reports(fx, target, 0, 5);
    const auto tx1 = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60, uint256::ONE,
                                                   reports, fx.list, legacy);
    const auto tx2 = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60, uint256::ONE,
                                                   reports, fx.list, current);
    BOOST_CHECK(tx1.msgHash != tx2.msgHash);
}

// The point of the format: applying an unobserved bit changes nothing. Not the
// counter, not a suspension, not a ban. Observed online still heals, observed
// missed still counts.
BOOST_AUTO_TEST_CASE(unobserved_is_neutral)
{
    auto [list, order] = MakeList(10);
    const auto params = ApplyParams();
    auto st = [&] { return list.GetMN(order[0])->pdmnState; };

    // three missed epochs, all observed
    for (uint32_t e = 1; e <= 3; ++e) {
        list.ApplyServiceCommitment(V2(e, 10, /*zero_missed=*/true, /*zero_unobserved=*/false), list, 100 + e, params, false);
    }
    BOOST_CHECK_EQUAL(st()->nMissedEpochs, 3u);

    // an epoch with no verdict on index 0: the counter neither climbs nor resets
    list.ApplyServiceCommitment(V2(4, 10, false, /*zero_unobserved=*/true), list, 104, params, false);
    BOOST_CHECK_EQUAL(st()->nMissedEpochs, 3u);
    BOOST_CHECK(!st()->fRewardSuspended);
    BOOST_CHECK_EQUAL(st()->nLastServiceEpoch, 4u); // the epoch itself is recorded

    // the streak resumes from where it was: one more observed miss suspends
    list.ApplyServiceCommitment(V2(5, 10, true, false), list, 105, params, false);
    BOOST_CHECK_EQUAL(st()->nMissedEpochs, 4u);
    BOOST_CHECK(st()->fRewardSuspended);

    // unobserved while suspended: still suspended, not revived, not banned
    list.ApplyServiceCommitment(V2(6, 10, false, true), list, 106, params, false);
    BOOST_CHECK_EQUAL(st()->nMissedEpochs, 4u);
    BOOST_CHECK(st()->fRewardSuspended);
    BOOST_CHECK(!st()->IsBanned());

    // one more observed miss bans
    list.ApplyServiceCommitment(V2(7, 10, true, false), list, 107, params, false);
    BOOST_CHECK(st()->IsBanned());
    BOOST_CHECK_EQUAL(st()->nDSLBanHeight, 107);

    // unobserved while banned: the ban stands -- no evidence is not a revival
    list.ApplyServiceCommitment(V2(8, 10, false, true), list, 108, params, false);
    BOOST_CHECK(st()->IsBanned());
    BOOST_CHECK_EQUAL(st()->nDSLBanHeight, 107);

    // an observed online verdict, and only that, revives and resets
    list.ApplyServiceCommitment(V2(9, 10, false, false), list, 109, params, false);
    BOOST_CHECK(!st()->IsBanned());
    BOOST_CHECK_EQUAL(st()->nMissedEpochs, 0u);
    BOOST_CHECK(!st()->fRewardSuspended);

    // the other nine were observed online throughout and never touched
    for (size_t i = 1; i < 10; ++i) {
        BOOST_CHECK_EQUAL(list.GetMN(order[i])->pdmnState->nMissedEpochs, 0u);
        BOOST_CHECK(!list.GetMN(order[i])->pdmnState->IsBanned());
    }
}

// Version 1 keeps its meaning for the history that carries it: a clear bit
// heals, exactly as it did the day those blocks were mined.
BOOST_AUTO_TEST_CASE(legacy_commitment_still_heals)
{
    auto [list, order] = MakeList(10);
    const auto params = ApplyParams();
    for (uint32_t e = 1; e <= 5; ++e) {
        list.ApplyServiceCommitment(V2(e, 10, true, false), list, 100 + e, params, false);
    }
    BOOST_CHECK(list.GetMN(order[0])->pdmnState->IsBanned());

    CPoSeServiceCommitment legacy;
    legacy.nVersion = CPoSeServiceCommitment::LEGACY_VERSION;
    legacy.nEpoch = 6;
    legacy.missed.assign(10, false); // no observed field: everyone is judged online
    list.ApplyServiceCommitment(legacy, list, 106, params, false);
    const auto st = list.GetMN(order[0])->pdmnState;
    BOOST_CHECK(!st->IsBanned());
    BOOST_CHECK_EQUAL(st->nMissedEpochs, 0u);
}

// A version-2 commitment whose observed field is the wrong length is not
// applied at all, rather than applied over a mismatched index.
BOOST_AUTO_TEST_CASE(mismatched_observed_length_is_not_applied)
{
    auto [list, order] = MakeList(10);
    const auto params = ApplyParams();
    auto c = V2(1, 10, true, false);
    c.observed.resize(9);
    list.ApplyServiceCommitment(c, list, 101, params, false);
    BOOST_CHECK_EQUAL(list.GetMN(order[0])->pdmnState->nMissedEpochs, 0u);
    BOOST_CHECK_EQUAL(list.GetMN(order[0])->pdmnState->nLastServiceEpoch, 0u);
}

// The bitfield invariants consensus checks: same length, and missed implies
// observed. Version 1 has nothing to check.
BOOST_AUTO_TEST_CASE(bitfield_rules_are_enforced)
{
    TxValidationState state;

    auto ok = V2(1, 7, true, false);
    BOOST_CHECK(CheckServiceCommitmentBitfields(ok, state));

    auto short_observed = ok;
    short_observed.observed.resize(6);
    BOOST_CHECK(!CheckServiceCommitmentBitfields(short_observed, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-dsl-bitfield-size");

    state = TxValidationState();
    auto missed_unobserved = V2(1, 7, true, true); // index 0 missed but not observed
    BOOST_CHECK(!CheckServiceCommitmentBitfields(missed_unobserved, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-dsl-missed-unobserved");

    state = TxValidationState();
    CPoSeServiceCommitment legacy;
    legacy.nVersion = CPoSeServiceCommitment::LEGACY_VERSION;
    legacy.missed.assign(7, true);
    BOOST_CHECK(CheckServiceCommitmentBitfields(legacy, state));
}

// The observed bits travel under the threshold signature: they are part of the
// serialized transaction the quorum signs, so flipping one after signing changes
// the message hash.
BOOST_AUTO_TEST_CASE(observed_bits_are_under_the_signature)
{
    auto fx = MakeFixture(30);
    const uint256 target = TaggedHash(3, 0, "protx");
    const auto params = AggParams(0);
    const auto reports = Reports(fx, target, 5, 0);
    auto a = dsl::BuildServiceCommitmentTx(500, fx.epoch, Consensus::LLMQType::LLMQ_50_60, uint256::ONE,
                                           reports, fx.list, params);
    // rebuild the transaction with one observed bit cleared and nothing else
    CPoSeServiceCommitmentTxPayload payload;
    payload.commitment = a.commitment;
    payload.commitment.observed[CanonicalIndex(fx.list, target)] = false;
    CMutableTransaction tx(a.tx);
    SetTxPayload(tx, payload);
    BOOST_CHECK(tx.GetHash() != a.msgHash);
}

BOOST_AUTO_TEST_SUITE_END()
