// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// DeFCon Sentinel Layer (DSL / "Service PoSe") scale simulation.
//
// This harness compares, on identical synthetic CDeterministicMNList fixtures,
// the time-to-PoSe-ban of a stopped masternode under two mechanisms:
//
//   * DKG-PoSe (the status quo): a stopped MN is only punished when it is
//     selected into a DKG quorum and fails it. Selection is size/N per round,
//     so the ban time grows with N. Modelled by calling the PRODUCTION
//     CDeterministicMNList::CalculateQuorum() and accruing the real penalty.
//
//   * DSL: every epoch, 7 deterministically-selected sentinels probe the MN;
//     >=nDSLSentinelAgree agreeing observations set the MISSED bit;
//     nDSLBanEpochs consecutive MISSED epochs ban it. Sentinel selection calls
//     the PRODUCTION CalculateScores().
//     Ban time is independent of N.
//
// It also measures the false-positive rate (a healthy-but-flaky node wrongly
// suspended or banned) at a configurable network availability.
//
// Env: DEFCON_SIM_SEED, DEFCON_DSL_POPULATIONS, DEFCON_DSL_EPOCHS,
//      DEFCON_DSL_DKG_EPOCHS, DEFCON_DSL_DOWN_SAMPLE, DEFCON_SIM_OUTPUT_DIR.

#include <consensus/params.h>
#include <evo/deterministicmns.h>

#include <hash.h>
#include <pubkey.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---- Q60 profile and DSL rule constants.
// The rule constants are read from Consensus::Params' OWN default member
// initialisers, not copied as literals, so this harness cannot drift from the
// rule the node ships. An earlier revision hardcoded them and did drift: it
// measured suspend=2/ban=3/mass-outage=25% against a node shipping 4/5/15%.
constexpr size_t Q60_SIZE{60};
const Consensus::Params DSL_NODE_DEFAULTS{};
const size_t DSL_SENTINELS{static_cast<size_t>(DSL_NODE_DEFAULTS.nDSLSentinelCount)};
const size_t DSL_MIN_AGREE{static_cast<size_t>(DSL_NODE_DEFAULTS.nDSLSentinelAgree)};
const int DSL_MASS_OUTAGE_PCT{DSL_NODE_DEFAULTS.nDSLMassOutagePct};
const int DKG_INTERVAL_BLOCKS{DSL_NODE_DEFAULTS.nDSLEpochInterval}; // one epoch, one DKG round

uint64_t ReadEnvU64(const char* name, uint64_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::stoull(value);
}

std::vector<size_t> ReadPopulations()
{
    const char* raw = std::getenv("DEFCON_DSL_POPULATIONS");
    const std::string input = raw == nullptr ? "150,500,1500" : raw;
    std::vector<size_t> out;
    std::stringstream ss{input};
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        const size_t p = std::stoull(tok);
        BOOST_REQUIRE_MESSAGE(p >= Q60_SIZE && p <= 15000, "population in [60,15000]");
        out.push_back(p);
    }
    BOOST_REQUIRE(!out.empty());
    return out;
}

uint256 TaggedHash(uint64_t a, uint64_t b, uint64_t c, std::string_view tag)
{
    CHashWriter w{SER_NETWORK, 0};
    w << a << b << c << std::string{tag};
    return w.GetHash();
}

// A synthetic but genuine MN list: confirmedHash set, so every MN is eligible
// for the production selection functions (no grinding).
CDeterministicMNList BuildList(size_t population, uint64_t seed)
{
    CDeterministicMNList list{TaggedHash(seed, 0, population, "list-block"), 1,
                              static_cast<uint32_t>(population)};
    for (size_t i = 0; i < population; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        dmn->proTxHash = TaggedHash(seed, 0, i, "protx");
        dmn->collateralOutpoint = COutPoint{TaggedHash(seed, 0, i, "collateral"), 0};
        auto st = std::make_shared<CDeterministicMNState>();
        st->nRegisteredHeight = 1;
        st->UpdateConfirmedHash(dmn->proTxHash, TaggedHash(seed, 0, i, "confirmed"));
        uint160 owner;
        std::copy_n(dmn->proTxHash.begin(), owner.size(), owner.begin());
        st->keyIDOwner = CKeyID{owner};
        dmn->pdmnState = st;
        list.AddMN(dmn);
    }
    return list;
}

// The DSL sentinel picker: production CalculateScores() with a per-target,
// per-epoch modifier, taking the lowest-scoring MNs excluding the target.
std::vector<uint64_t> CalcSentinels(const CDeterministicMNList& list,
                                    const uint256& targetProTxHash, uint64_t targetId,
                                    const uint256& epochBlockHash, size_t count)
{
    CHashWriter w{SER_NETWORK, 0};
    w << targetProTxHash << epochBlockHash;
    const uint256 modifier = w.GetHash();
    auto scores = list.CalculateScores(modifier); // ascending arith256 sort inside
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> out;
    for (const auto& s : scores) {
        const uint64_t id = s.second->GetInternalId();
        if (id == targetId) continue;
        out.push_back(id);
        if (out.size() == count) break;
    }
    return out;
}

// deterministic per-(node,epoch) availability draw
bool NodeUp(uint64_t seed, uint64_t epoch, uint64_t node, int availability_pct)
{
    const uint64_t r = TaggedHash(seed, epoch, node, "up").GetUint64(0) % 10000;
    return r < static_cast<uint64_t>(availability_pct) * 100;
}

struct Row {
    size_t population;
    int availability_pct;
    // DKG-PoSe (status quo)
    uint64_t dkg_median, dkg_p90, dkg_p99;
    // DSL
    uint64_t dsl_median, dsl_p90, dsl_p99;
    double dsl_unknown_frac;
    // false-positive at this availability
    double false_suspend_frac, false_ban_frac;
    uint64_t mass_outage_epochs;
};

uint64_t Pctl(std::vector<uint64_t> v, double q)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t idx = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1)));
    return v[idx];
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(dsl_service_pose_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(dsl_ban_time_vs_dkg_pose)
{
    const uint64_t seed = ReadEnvU64("DEFCON_SIM_SEED", 12648430);
    const uint32_t DSL_SUSPEND_EPOCHS = static_cast<uint32_t>(ReadEnvU64("DEFCON_DSL_SUSPEND_EPOCHS", DSL_NODE_DEFAULTS.nDSLSuspendEpochs));
    const uint32_t DSL_BAN_EPOCHS = static_cast<uint32_t>(ReadEnvU64("DEFCON_DSL_BAN_EPOCHS", DSL_NODE_DEFAULTS.nDSLBanEpochs));
    const uint64_t dsl_epochs = ReadEnvU64("DEFCON_DSL_EPOCHS", 60);
    const uint64_t dkg_epochs = ReadEnvU64("DEFCON_DSL_DKG_EPOCHS", 4000);
    const uint64_t down_sample = ReadEnvU64("DEFCON_DSL_DOWN_SAMPLE", 400);
    const bool skip_fp = ReadEnvU64("DEFCON_DSL_SKIP_FP", 0) != 0;
    const int availabilities[]{99, 98, 95, 92};

    const char* out_env = std::getenv("DEFCON_SIM_OUTPUT_DIR");
    const std::filesystem::path out_dir = out_env == nullptr
        ? std::filesystem::path{"dsl-sim-results"} : std::filesystem::path{out_env};
    std::filesystem::create_directories(out_dir);
    std::ofstream csv{out_dir / "dsl_results.csv", std::ios::out | std::ios::trunc};
    csv << "population,availability_pct,dkg_median,dkg_p90,dkg_p99,"
           "dsl_median,dsl_p90,dsl_p99,dsl_unknown_frac,"
           "false_suspend_frac,false_ban_frac,mass_outage_epochs\n";

    for (const size_t population : ReadPopulations()) {
        const CDeterministicMNList list = BuildList(population, seed);
        BOOST_REQUIRE_EQUAL(list.GetAllMNsCount(), population);
        const size_t sample = std::min<size_t>(down_sample, population);

        // proTxHash by internalId, for the sentinel picker.
        std::vector<uint256> protx(population);
        list.ForEachMN(false, [&](const auto& dmn) { protx[dmn.GetInternalId()] = dmn.proTxHash; });

        // ---- DKG-PoSe track (per population; availability-independent). A
        // stopped node is punished only when the real quorum selects it. Penalty
        // accrues CalcPenalty(66); DecreaseScores() removes 1/block => -24/epoch.
        const int max_penalty = std::max<int>(100, static_cast<int>(population));
        const int fail_penalty = (max_penalty * 66) / 100;
        std::vector<int> dkg_penalty(sample, 0);
        std::vector<int64_t> dkg_ban(sample, -1);
        for (uint64_t e = 0; e < dkg_epochs; ++e) {
            const uint256 modifier = TaggedHash(seed, e, population, "dkg");
            const auto quorum = list.CalculateQuorum(Q60_SIZE, modifier);
            std::vector<char> selected(sample, 0);
            for (const auto& m : quorum) {
                const uint64_t id = m->GetInternalId();
                if (id < sample) selected[id] = 1;
            }
            bool any_left = false;
            for (size_t i = 0; i < sample; ++i) {
                if (dkg_ban[i] != -1) continue;
                any_left = true;
                if (selected[i]) dkg_penalty[i] = std::min(max_penalty, dkg_penalty[i] + fail_penalty);
                else dkg_penalty[i] = std::max(0, dkg_penalty[i] - DKG_INTERVAL_BLOCKS);
                if (dkg_penalty[i] >= max_penalty) dkg_ban[i] = static_cast<int64_t>(e) + 1;
            }
            if (!any_left) break;
        }

        for (const int availability : availabilities) {
            Row row{};
            row.population = population;
            row.availability_pct = availability;

            // ---- DSL track: a stopped node's 7 sentinels probe it each epoch.
            std::vector<uint32_t> missed_run(sample, 0);
            std::vector<int64_t> dsl_ban(sample, -1);
            uint64_t unknown_epochs = 0, total_target_epochs = 0;
            for (uint64_t e = 0; e < dsl_epochs; ++e) {
                const uint256 epoch_hash = TaggedHash(seed, e, population, "epoch");
                bool any_left = false;
                for (size_t i = 0; i < sample; ++i) {
                    if (dsl_ban[i] != -1) continue;
                    any_left = true;
                    ++total_target_epochs;
                    const auto sentinels = CalcSentinels(list, protx[i], i, epoch_hash, DSL_SENTINELS);
                    size_t reachable = 0, reporting_missed = 0;
                    for (const uint64_t s : sentinels) {
                        if (!NodeUp(seed, e, s, availability)) continue; // sentinel offline: no report
                        ++reachable;
                        ++reporting_missed; // the target is stopped, so every up sentinel sees MISSED
                    }
                    // The node has no UNKNOWN state: BuildServiceCommitment sets
                    // the bit iff missedCount >= nDSLSentinelAgree, and a false bit
                    // RESETS nMissedEpochs (deterministicmns.cpp:430-431). Too few
                    // reachable sentinels therefore breaks the run rather than
                    // preserving it -- which is what lengthens the p90/p99 tail.
                    if (reporting_missed < DSL_MIN_AGREE) ++unknown_epochs;
                    if (reporting_missed >= DSL_MIN_AGREE) {
                        if (++missed_run[i] >= DSL_BAN_EPOCHS) dsl_ban[i] = static_cast<int64_t>(e) + 1;
                    } else {
                        missed_run[i] = 0;
                    }
                }
                if (!any_left) break;
            }
            row.dsl_unknown_frac = total_target_epochs ? double(unknown_epochs) / double(total_target_epochs) : 0.0;

            if (!skip_fp) {
            // ---- False-positive: healthy-but-flaky nodes at this availability.
            // A node is MISSED an epoch iff it is itself down AND >=nDSLSentinelAgree
            // sentinels are up to observe it. Suspend/ban at the node's own thresholds.
            std::vector<uint32_t> fp_run(population, 0);
            uint64_t false_suspends = 0, false_bans = 0, mass_outage_epochs = 0;
            const uint64_t fp_epochs = std::max<uint64_t>(dsl_epochs, 500);
            for (uint64_t e = 0; e < fp_epochs; ++e) {
                const uint256 epoch_hash = TaggedHash(seed, e, population, "fp-epoch");
                size_t missed_this_epoch = 0;
                std::vector<char> is_missed(population, 0);
                for (size_t i = 0; i < population; ++i) {
                    if (NodeUp(seed, e, i, availability)) continue; // the node is up: never MISSED
                    // node i is down this epoch; are >=5 of its sentinels up to observe?
                    const auto sentinels = CalcSentinels(list, protx[i], i, epoch_hash, DSL_SENTINELS);
                    size_t up = 0;
                    for (const uint64_t s : sentinels) if (NodeUp(seed, e, s, availability)) ++up;
                    if (up >= DSL_MIN_AGREE) { is_missed[i] = 1; ++missed_this_epoch; }
                }
                const bool mass_outage = missed_this_epoch * 100 >= population * DSL_MASS_OUTAGE_PCT;
                if (mass_outage) { ++mass_outage_epochs; continue; } // guard: no penalties this epoch
                for (size_t i = 0; i < population; ++i) {
                    if (is_missed[i]) {
                        const uint32_t r = ++fp_run[i];
                        if (r == DSL_SUSPEND_EPOCHS) ++false_suspends;
                        if (r == DSL_BAN_EPOCHS) ++false_bans;
                    } else {
                        fp_run[i] = 0;
                    }
                }
            }
            row.false_suspend_frac = double(false_suspends) / double(population);
            row.false_ban_frac = double(false_bans) / double(population);
            row.mass_outage_epochs = mass_outage_epochs;
            }

            // percentiles (unbanned samples counted at the horizon so the tail is honest)
            auto finalize = [](std::vector<int64_t> bans, uint64_t horizon) {
                std::vector<uint64_t> v;
                v.reserve(bans.size());
                for (int64_t b : bans) v.push_back(b == -1 ? horizon + 1 : static_cast<uint64_t>(b));
                return v;
            };
            const auto dkgv = finalize(dkg_ban, dkg_epochs);
            const auto dslv = finalize(dsl_ban, dsl_epochs);
            row.dkg_median = Pctl(dkgv, 0.5); row.dkg_p90 = Pctl(dkgv, 0.9); row.dkg_p99 = Pctl(dkgv, 0.99);
            row.dsl_median = Pctl(dslv, 0.5); row.dsl_p90 = Pctl(dslv, 0.9); row.dsl_p99 = Pctl(dslv, 0.99);

            csv << row.population << ',' << row.availability_pct << ','
                << row.dkg_median << ',' << row.dkg_p90 << ',' << row.dkg_p99 << ','
                << row.dsl_median << ',' << row.dsl_p90 << ',' << row.dsl_p99 << ','
                << row.dsl_unknown_frac << ',' << row.false_suspend_frac << ','
                << row.false_ban_frac << ',' << row.mass_outage_epochs << '\n';

            BOOST_TEST_MESSAGE("pop=" << population << " avail=" << availability
                << "% | DKG ban epochs med/p90/p99=" << row.dkg_median << '/' << row.dkg_p90 << '/' << row.dkg_p99
                << " | DSL=" << row.dsl_median << '/' << row.dsl_p90 << '/' << row.dsl_p99
                << " | false_ban=" << row.false_ban_frac);
        }
    }
    csv.close();
    BOOST_TEST_MESSAGE("DSL results written to " << (out_dir / "dsl_results.csv").string());
}


// ---------------------------------------------------------------------------
// Correlated outage vs the mass-outage guard (design decision 6). A provider/
// region group of `group_pct` of the network goes down together for a burst
// longer than the ban window; measure what fraction of that group is wrongly
// banned for each guard threshold.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(dsl_correlated_outage_guard)
{
    const uint64_t seed = ReadEnvU64("DEFCON_SIM_SEED", 12648430);
    const uint32_t ban_epochs = static_cast<uint32_t>(ReadEnvU64("DEFCON_DSL_BAN_EPOCHS", DSL_NODE_DEFAULTS.nDSLBanEpochs));
    const int group_pcts[]{10, 20, 30, 40};
    const int guard_pcts[]{15, 20, 25, 30};
    const uint64_t burst = ReadEnvU64("DEFCON_DSL_BURST", 8);
    const uint64_t epochs = 40;
    const int base_avail = 99;

    const char* out_env = std::getenv("DEFCON_SIM_OUTPUT_DIR");
    const std::filesystem::path out_dir = out_env == nullptr
        ? std::filesystem::path{"dsl-sim-results"} : std::filesystem::path{out_env};
    std::filesystem::create_directories(out_dir);
    std::ofstream csv{out_dir / "correlated_guard.csv", std::ios::out | std::ios::trunc};
    csv << "population,group_pct,guard_pct,group_false_banned_frac,frozen_epochs\n";

    for (const size_t population : ReadPopulations()) {
        const CDeterministicMNList list = BuildList(population, seed);
        std::vector<uint256> protx(population);
        list.ForEachMN(false, [&](const auto& dmn) { protx[dmn.GetInternalId()] = dmn.proTxHash; });

        for (const int group_pct : group_pcts) {
            const size_t G = population * group_pct / 100;
            for (const int guard_pct : guard_pcts) {
                std::vector<uint32_t> run(population, 0);
                std::vector<char> banned(population, 0);
                uint64_t frozen = 0;
                auto down = [&](size_t i, uint64_t e) {
                    if (i < G && e >= 5 && e < 5 + burst) return true;
                    return !NodeUp(seed, e, i, base_avail);
                };
                for (uint64_t e = 0; e < epochs; ++e) {
                    const uint256 eh = TaggedHash(seed, e, population, "co-epoch");
                    std::vector<char> is_missed(population, 0);
                    size_t missed = 0;
                    for (size_t i = 0; i < population; ++i) {
                        if (!down(i, e)) continue;
                        const auto sent = CalcSentinels(list, protx[i], i, eh, DSL_SENTINELS);
                        size_t up = 0;
                        for (const uint64_t sdx : sent) if (!down(sdx, e)) ++up;
                        if (up >= DSL_MIN_AGREE) { is_missed[i] = 1; ++missed; }
                    }
                    if (missed * 100 >= population * static_cast<size_t>(guard_pct)) { ++frozen; continue; }
                    for (size_t i = 0; i < population; ++i) {
                        if (banned[i]) continue;
                        if (is_missed[i]) { if (++run[i] >= ban_epochs) banned[i] = 1; }
                        else run[i] = 0;
                    }
                }
                size_t group_banned = 0;
                for (size_t i = 0; i < G; ++i) if (banned[i]) ++group_banned;
                const double frac = G ? double(group_banned) / double(G) : 0.0;
                csv << population << ',' << group_pct << ',' << guard_pct << ','
                    << frac << ',' << frozen << '\n';
                BOOST_TEST_MESSAGE("pop=" << population << " group=" << group_pct
                    << "% guard=" << guard_pct << "% => group_false_banned=" << frac
                    << " frozen_epochs=" << frozen);
            }
        }
    }
    csv.close();
}

// ---------------------------------------------------------------------------
// Sentinel capture / sybil resistance (design decision 5 + security section D).
// An attacker controls `concentration` of the network. SHIELD: keep its own
// dead node from being banned by forcing UNKNOWN. GRIEF: get an honest live
// node falsely banned by lying MISSED. Swept over 7 vs 11 sentinels, using the
// production CalculateScores() rotation.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(dsl_sentinel_capture)
{
    const uint64_t seed = ReadEnvU64("DEFCON_SIM_SEED", 12648430);
    const uint32_t ban_epochs = static_cast<uint32_t>(ReadEnvU64("DEFCON_DSL_BAN_EPOCHS", DSL_NODE_DEFAULTS.nDSLBanEpochs));
    const int conc_pcts[]{5, 10, 15, 20, 30};
    const size_t sentinel_counts[]{7, 11};
    const uint64_t epochs = 120;
    const size_t max_targets = ReadEnvU64("DEFCON_DSL_TARGETS", 300);

    const char* out_env = std::getenv("DEFCON_SIM_OUTPUT_DIR");
    const std::filesystem::path out_dir = out_env == nullptr
        ? std::filesystem::path{"dsl-sim-results"} : std::filesystem::path{out_env};
    std::filesystem::create_directories(out_dir);
    std::ofstream csv{out_dir / "sentinel_capture.csv", std::ios::out | std::ios::trunc};
    csv << "population,concentration_pct,sentinels,agree,shield_ban_prevented_frac,grief_false_ban_frac\n";

    for (const size_t population : ReadPopulations()) {
        const CDeterministicMNList list = BuildList(population, seed);
        std::vector<uint256> protx(population);
        list.ForEachMN(false, [&](const auto& dmn) { protx[dmn.GetInternalId()] = dmn.proTxHash; });

        for (const int conc_pct : conc_pcts) {
            const size_t C = population * conc_pct / 100;
            for (const size_t sc : sentinel_counts) {
                const size_t agree = (sc * DSL_MIN_AGREE + (DSL_SENTINELS - 1)) / DSL_SENTINELS;

                size_t shield_targets = 0, shielded = 0;
                for (size_t t = 0; t < C && shield_targets < max_targets; ++t) {
                    ++shield_targets;
                    uint32_t r = 0; bool banned = false;
                    for (uint64_t e = 0; e < epochs && !banned; ++e) {
                        const auto sent = CalcSentinels(list, protx[t], t,
                                                        TaggedHash(seed, e, population, "cap"), sc);
                        size_t ctrl = 0;
                        for (const uint64_t sdx : sent) if (sdx < C) ++ctrl;
                        const bool verdict_missed = (sc - ctrl) >= agree;
                        if (verdict_missed) { if (++r >= ban_epochs) banned = true; } else r = 0;
                    }
                    if (!banned) ++shielded;
                }
                const double shield_frac = shield_targets ? double(shielded) / double(shield_targets) : 0.0;

                size_t grief_targets = 0, griefed = 0;
                for (size_t t = C; t < population && grief_targets < max_targets; ++t) {
                    ++grief_targets;
                    uint32_t r = 0; bool banned = false;
                    for (uint64_t e = 0; e < epochs && !banned; ++e) {
                        const auto sent = CalcSentinels(list, protx[t], t,
                                                        TaggedHash(seed, e, population, "cap"), sc);
                        size_t ctrl = 0;
                        for (const uint64_t sdx : sent) if (sdx < C) ++ctrl;
                        const bool verdict_missed = ctrl >= agree;
                        if (verdict_missed) { if (++r >= ban_epochs) banned = true; } else r = 0;
                    }
                    if (banned) ++griefed;
                }
                const double grief_frac = grief_targets ? double(griefed) / double(grief_targets) : 0.0;

                csv << population << ',' << conc_pct << ',' << sc << ',' << agree << ','
                    << shield_frac << ',' << grief_frac << '\n';
                BOOST_TEST_MESSAGE("pop=" << population << " conc=" << conc_pct
                    << "% sentinels=" << sc << " agree=" << agree
                    << " => shield_prevented=" << shield_frac << " grief_false_ban=" << grief_frac);
            }
        }
    }
    csv.close();
}

BOOST_AUTO_TEST_SUITE_END()
