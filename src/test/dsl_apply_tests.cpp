// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/pose_service.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_apply_tests, BasicTestingSetup)

namespace {
// A masternode list of `n` members with distinct owner keys; also returns the
// proTxHashes in the canonical (ascending) order the bitfield indexes.
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

Consensus::Params DslParams(int enforcement_height)
{
    Consensus::Params p;
    p.nDSLSuspendEpochs = 4;
    p.nDSLBanEpochs = 5;
    p.nDSLMassOutagePct = 15;
    p.nDSLEnforcementHeight = enforcement_height;
    return p;
}

// A commitment marking the single canonical index `missed_idx` (or none if -1).
CPoSeServiceCommitment MissOne(uint32_t epoch, size_t n, int missed_idx)
{
    CPoSeServiceCommitment c;
    c.nEpoch = epoch;
    c.missed.assign(n, false);
    if (missed_idx >= 0) c.missed[static_cast<size_t>(missed_idx)] = true;
    return c;
}
} // namespace

BOOST_AUTO_TEST_CASE(suspends_at_four_bans_at_five)
{
    auto [list, order] = MakeList(10);
    const auto params = DslParams(/*enforcement=*/0); // enforcing
    auto st = [&] { return list.GetMN(order[0])->pdmnState; };

    for (uint32_t e = 1; e <= 3; ++e) {
        list.ApplyServiceCommitment(MissOne(e, 10, 0), list, /*nHeight=*/100 + e, params, false);
        BOOST_CHECK_EQUAL(st()->nMissedEpochs, e);
        BOOST_CHECK(!st()->fRewardSuspended);
        BOOST_CHECK(!st()->IsBanned());
    }
    list.ApplyServiceCommitment(MissOne(4, 10, 0), list, 104, params, false);
    BOOST_CHECK(st()->fRewardSuspended);            // suspended at 4
    BOOST_CHECK(!st()->IsBanned());
    BOOST_CHECK_EQUAL(list.GetEffectivePaymentWeight(*list.GetMN(order[0])), 0); // earns nothing

    list.ApplyServiceCommitment(MissOne(5, 10, 0), list, 105, params, false);
    BOOST_CHECK(st()->IsBanned());                  // banned at 5
    BOOST_CHECK_EQUAL(st()->nDSLBanHeight, 105);
}

// Below the enforcement height the counter advances (for the record) but no
// economic penalty is applied -- shadow mode.
BOOST_AUTO_TEST_CASE(shadow_records_without_penalising)
{
    auto [list, order] = MakeList(10);
    const auto params = DslParams(/*enforcement=*/std::numeric_limits<int>::max());
    for (uint32_t e = 1; e <= 6; ++e) {
        list.ApplyServiceCommitment(MissOne(e, 10, 0), list, 100 + e, params, false);
    }
    const auto st = list.GetMN(order[0])->pdmnState;
    BOOST_CHECK_EQUAL(st->nMissedEpochs, 6u);   // recorded
    BOOST_CHECK(!st->fRewardSuspended);         // but not penalised
    BOOST_CHECK(!st->IsBanned());
    BOOST_CHECK_EQUAL(st->nLastServiceEpoch, 6u);
}

// A missed fraction at or above the guard percentage freezes all penalties and
// leaves the counters untouched -- a correlated outage neither bans nor heals.
BOOST_AUTO_TEST_CASE(mass_outage_guard_freezes)
{
    auto [list, order] = MakeList(10);
    const auto params = DslParams(/*enforcement=*/0);
    // three of ten missed = 30% >= 15% guard
    CPoSeServiceCommitment c;
    c.nEpoch = 1;
    c.missed.assign(10, false);
    c.missed[0] = c.missed[1] = c.missed[2] = true;
    list.ApplyServiceCommitment(c, list, 101, params, false);
    for (size_t i = 0; i < 3; ++i) {
        BOOST_CHECK_EQUAL(list.GetMN(order[i])->pdmnState->nMissedEpochs, 0u); // frozen
        BOOST_CHECK(!list.GetMN(order[i])->pdmnState->IsBanned());
    }
    // but the epoch is still recorded
    BOOST_CHECK_EQUAL(list.GetMN(order[0])->pdmnState->nLastServiceEpoch, 1u);
}

// An online observation clears the counter, and a fresh online observation of a
// service-banned node revives it (the service path, not a ProUpServTx).
BOOST_AUTO_TEST_CASE(online_resets_and_service_revives)
{
    auto [list, order] = MakeList(10);
    const auto params = DslParams(/*enforcement=*/0);
    // drive it to a ban
    for (uint32_t e = 1; e <= 5; ++e) {
        list.ApplyServiceCommitment(MissOne(e, 10, 0), list, 100 + e, params, false);
    }
    BOOST_CHECK(list.GetMN(order[0])->pdmnState->IsBanned());

    // one online epoch: revived and reset
    list.ApplyServiceCommitment(MissOne(6, 10, -1), list, 106, params, false);
    const auto st = list.GetMN(order[0])->pdmnState;
    BOOST_CHECK(!st->IsBanned());
    BOOST_CHECK_EQUAL(st->nDSLBanHeight, -1);
    BOOST_CHECK_EQUAL(st->nMissedEpochs, 0u);
    BOOST_CHECK(!st->fRewardSuspended);
}

BOOST_AUTO_TEST_SUITE_END()
