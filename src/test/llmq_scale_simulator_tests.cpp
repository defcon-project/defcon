// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

/**
 * Core-native deterministic masternode/LLMQ scale simulator.
 *
 * This harness intentionally calls CDeterministicMNList::CalculateQuorum().
 * It must not contain a private reimplementation of the production selection
 * algorithm.
 *
 * Environment:
 *   DEFCON_SIM_POPULATIONS=150,300,500
 *   DEFCON_SIM_ROUNDS=100
 *   DEFCON_SIM_SEED=12648430
 *   DEFCON_SIM_OUTPUT_DIR=/tmp/defcon-llmq-sim
 */

#include <evo/deterministicmns.h>

#include <hash.h>
#include <pubkey.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Profile {
    const char* name;
    size_t size;
    size_t min_size;
    size_t threshold;
};

constexpr Profile PROFILES[]{
    {"q25_22_17", 25, 22, 17},
    {"q60_44_41", 60, 44, 41},
};

constexpr int OFFLINE_PCTS[]{5, 10, 15, 20, 25, 30};
constexpr int CONCENTRATION_PCTS[]{25, 33, 40};
constexpr int MIXED_VERSION_PCTS[]{10, 25, 40, 50};
constexpr int CORRELATED_GROUP_PCTS[]{10, 20, 30, 40};
constexpr size_t ACTIVE_QUORUM_WINDOW{4};

uint64_t ReadEnvU64(const char* name, uint64_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    try {
        size_t consumed{0};
        const uint64_t parsed = std::stoull(value, &consumed, 10);
        if (consumed != std::string(value).size()) {
            BOOST_FAIL(std::string{name} + " contains trailing characters");
        }
        return parsed;
    } catch (const std::exception& e) {
        BOOST_FAIL(std::string{"Invalid "} + name + ": " + e.what());
    }
    return fallback;
}

std::vector<size_t> ReadPopulations()
{
    const char* raw = std::getenv("DEFCON_SIM_POPULATIONS");
    const std::string input = raw == nullptr ? "150,300,500" : raw;
    std::vector<size_t> result;
    std::stringstream stream{input};
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) continue;
        const size_t population = std::stoull(token);
        if (population < 60 || population > 15000) {
            BOOST_FAIL("Population must be between 60 and 15000");
        }
        result.push_back(population);
    }
    if (result.empty()) BOOST_FAIL("No populations configured");
    return result;
}

uint256 TaggedHash(uint64_t seed, uint64_t round, uint64_t member, std::string_view tag)
{
    CHashWriter writer{SER_NETWORK, 0};
    writer << seed << round << member << std::string{tag};
    return writer.GetHash();
}

uint64_t Random64(uint64_t seed, uint64_t round, uint64_t member, std::string_view tag)
{
    return TaggedHash(seed, round, member, tag).GetUint64(0);
}

bool PercentEvent(uint64_t seed, uint64_t round, uint64_t member, std::string_view tag, int percent)
{
    return Random64(seed, round, member, tag) % 10000 < static_cast<uint64_t>(percent) * 100;
}

CDeterministicMNList BuildSyntheticList(size_t population, uint64_t seed)
{
    CDeterministicMNList list{TaggedHash(seed, 0, population, "list-block"), 1, static_cast<uint32_t>(population)};

    for (size_t i = 0; i < population; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        dmn->proTxHash = TaggedHash(seed, 0, i, "protx");
        dmn->collateralOutpoint = COutPoint{TaggedHash(seed, 0, i, "collateral"), 0};

        auto state = std::make_shared<CDeterministicMNState>();
        state->nRegisteredHeight = 1;
        state->UpdateConfirmedHash(dmn->proTxHash, TaggedHash(seed, 0, i, "confirmed"));

        uint160 owner;
        std::copy_n(dmn->proTxHash.begin(), owner.size(), owner.begin());
        state->keyIDOwner = CKeyID{owner};
        dmn->pdmnState = state;

        list.AddMN(dmn);
    }
    return list;
}

size_t CountOverlap(const std::vector<CDeterministicMNCPtr>& previous,
                    const std::vector<CDeterministicMNCPtr>& current)
{
    if (previous.empty()) return 0;
    size_t overlap{0};
    for (const auto& member : current) {
        if (std::any_of(previous.begin(), previous.end(), [&](const auto& old_member) {
                return old_member->proTxHash == member->proTxHash;
            })) {
            ++overlap;
        }
    }
    return overlap;
}

int ProviderFor(uint64_t seed, uint64_t member)
{
    // Deliberately skewed failure domains: 30%, 20%, 15%, then seven 5% groups.
    const uint64_t bucket = Random64(seed, 0, member, "provider") % 100;
    if (bucket < 30) return 0;
    if (bucket < 50) return 1;
    if (bucket < 65) return 2;
    return 3 + static_cast<int>((bucket - 65) / 5);
}

int AsnFor(uint64_t seed, uint64_t member)
{
    // Provider-correlated but not identical ASN allocation.
    return ProviderFor(seed, member) * 3 +
           static_cast<int>(Random64(seed, 0, member, "asn") % 3);
}

int RegionFor(uint64_t seed, uint64_t member)
{
    const uint64_t bucket = Random64(seed, 0, member, "region") % 100;
    if (bucket < 35) return 0;
    if (bucket < 60) return 1;
    if (bucket < 78) return 2;
    if (bucket < 90) return 3;
    return 4;
}

uint64_t OperatorFor(uint64_t seed, uint64_t member)
{
    const uint64_t bucket = Random64(seed, 0, member, "operator") % 100;
    if (bucket < 10) return 0;
    if (bucket < 15) return 1;
    if (bucket < 18) return 2;
    return 1000 + member;
}

uint64_t CollateralOwnerFor(uint64_t seed, uint64_t member)
{
    const uint64_t bucket = Random64(seed, 0, member, "collateral-owner") % 100;
    if (bucket < 8) return 0;
    if (bucket < 13) return 1;
    return 100000 + member;
}

int AvailabilityClassFor(uint64_t seed, uint64_t member)
{
    const uint64_t bucket = Random64(seed, 0, member, "availability-class") % 100;
    if (bucket < 70) return 0; // stable
    if (bucket < 90) return 1; // ordinary
    return 2;                  // fragile
}

struct Row {
    std::string scenario;
    size_t population;
    const Profile* profile;
    uint64_t round;
    int parameter;
    size_t selected;
    size_t valid;
    bool dkg_complete;
    bool signable;
    size_t adversarial;
    bool threshold_breach;
    size_t overlap;
};

class ResultWriter {
private:
    std::ofstream csv;
    std::ofstream jsonl;
    std::ofstream overlap_csv;
    std::ofstream overlap_jsonl;

public:
    explicit ResultWriter(const std::filesystem::path& output_dir)
    {
        std::filesystem::create_directories(output_dir);
        csv.open(output_dir / "results.csv", std::ios::out | std::ios::trunc);
        jsonl.open(output_dir / "results.jsonl", std::ios::out | std::ios::trunc);
        overlap_csv.open(output_dir / "overlap.csv", std::ios::out | std::ios::trunc);
        overlap_jsonl.open(output_dir / "overlap.jsonl", std::ios::out | std::ios::trunc);
        BOOST_REQUIRE_MESSAGE(csv.good(), "Unable to open CSV result file");
        BOOST_REQUIRE_MESSAGE(jsonl.good(), "Unable to open JSONL result file");
        BOOST_REQUIRE_MESSAGE(overlap_csv.good(), "Unable to open overlap CSV result file");
        BOOST_REQUIRE_MESSAGE(overlap_jsonl.good(), "Unable to open overlap JSONL result file");
        csv << "scenario,population,profile,round,parameter,selected,valid,"
               "dkg_complete,signable,adversarial,threshold_breach,overlap\n";
        overlap_csv << "population,profile,round,expected_consecutive_overlap,"
                       "observed_consecutive_overlap,active_window,repeated_members,"
                       "max_provider_members,max_asn_members,max_operator_members,"
                       "max_collateral_owner_members,top_provider_overlap\n";
    }

    void Write(const Row& row)
    {
        csv << row.scenario << ',' << row.population << ',' << row.profile->name << ','
            << row.round << ',' << row.parameter << ',' << row.selected << ',' << row.valid << ','
            << row.dkg_complete << ',' << row.signable << ',' << row.adversarial << ','
            << row.threshold_breach << ',' << row.overlap << '\n';

        jsonl << "{\"scenario\":\"" << row.scenario
              << "\",\"population\":" << row.population
              << ",\"profile\":\"" << row.profile->name
              << "\",\"round\":" << row.round
              << ",\"parameter\":" << row.parameter
              << ",\"selected\":" << row.selected
              << ",\"valid\":" << row.valid
              << ",\"dkg_complete\":" << (row.dkg_complete ? "true" : "false")
              << ",\"signable\":" << (row.signable ? "true" : "false")
              << ",\"adversarial\":" << row.adversarial
              << ",\"threshold_breach\":" << (row.threshold_breach ? "true" : "false")
              << ",\"overlap\":" << row.overlap << "}\n";
    }

    void WriteOverlap(size_t population,
                      const Profile& profile,
                      uint64_t round,
                      double expected_consecutive_overlap,
                      size_t observed_consecutive_overlap,
                      size_t active_window,
                      size_t repeated_members,
                      size_t max_provider_members,
                      size_t max_asn_members,
                      size_t max_operator_members,
                      size_t max_collateral_owner_members,
                      size_t top_provider_overlap)
    {
        overlap_csv << population << ',' << profile.name << ',' << round << ','
                    << std::setprecision(12) << expected_consecutive_overlap << ','
                    << observed_consecutive_overlap << ',' << active_window << ','
                    << repeated_members << ',' << max_provider_members << ','
                    << max_asn_members << ',' << max_operator_members << ','
                    << max_collateral_owner_members << ',' << top_provider_overlap << '\n';
        overlap_jsonl << "{\"population\":" << population
                      << ",\"profile\":\"" << profile.name
                      << "\",\"round\":" << round
                      << ",\"expected_consecutive_overlap\":" << std::setprecision(12)
                      << expected_consecutive_overlap
                      << ",\"observed_consecutive_overlap\":" << observed_consecutive_overlap
                      << ",\"active_window\":" << active_window
                      << ",\"repeated_members\":" << repeated_members
                      << ",\"max_provider_members\":" << max_provider_members
                      << ",\"max_asn_members\":" << max_asn_members
                      << ",\"max_operator_members\":" << max_operator_members
                      << ",\"max_collateral_owner_members\":" << max_collateral_owner_members
                      << ",\"top_provider_overlap\":" << top_provider_overlap << "}\n";
    }
};

template <typename KeyFn>
size_t MaxGroupMembers(const std::vector<CDeterministicMNCPtr>& quorum, KeyFn&& key_fn)
{
    std::map<uint64_t, size_t> counts;
    for (const auto& member : quorum) {
        ++counts[static_cast<uint64_t>(key_fn(member->GetInternalId()))];
    }
    size_t maximum{0};
    for (const auto& [_, count] : counts) maximum = std::max(maximum, count);
    return maximum;
}

Row MakeAvailabilityRow(std::string scenario,
                        size_t population,
                        const Profile& profile,
                        uint64_t round,
                        int parameter,
                        size_t selected,
                        size_t valid,
                        size_t overlap)
{
    return Row{
        std::move(scenario),
        population,
        &profile,
        round,
        parameter,
        selected,
        valid,
        valid >= profile.min_size,
        valid >= profile.threshold,
        0,
        false,
        overlap,
    };
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_scale_simulator_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(core_native_selection_and_fault_matrix)
{
    const uint64_t seed = ReadEnvU64("DEFCON_SIM_SEED", 12648430);
    const uint64_t rounds = ReadEnvU64("DEFCON_SIM_ROUNDS", 100);
    BOOST_REQUIRE_MESSAGE(rounds > 0 && rounds <= 1000000, "Rounds must be in [1, 1000000]");

    const char* output_env = std::getenv("DEFCON_SIM_OUTPUT_DIR");
    const std::filesystem::path output_dir =
        output_env == nullptr ? std::filesystem::path{"llmq-sim-results"} : output_env;
    ResultWriter writer{output_dir};

    for (const size_t population : ReadPopulations()) {
        const CDeterministicMNList mn_list = BuildSyntheticList(population, seed);
        BOOST_REQUIRE_EQUAL(mn_list.GetAllMNsCount(), population);

        for (const Profile& profile : PROFILES) {
            std::vector<CDeterministicMNCPtr> previous;
            std::vector<std::vector<CDeterministicMNCPtr>> active_history;
            std::vector<uint64_t> selection_counts(population, 0);
            double overlap_sum{0.0};

            for (uint64_t round = 0; round < rounds; ++round) {
                const uint256 modifier = TaggedHash(seed, round, population, profile.name);
                const auto quorum = mn_list.CalculateQuorum(profile.size, modifier);
                BOOST_REQUIRE_EQUAL(quorum.size(), profile.size);

                for (const auto& member : quorum) {
                    BOOST_REQUIRE(member->GetInternalId() < population);
                    ++selection_counts[member->GetInternalId()];
                }

                const size_t overlap = CountOverlap(previous, quorum);
                if (!previous.empty()) overlap_sum += overlap;

                std::map<uint256, size_t> active_member_counts;
                for (const auto& old_quorum : active_history) {
                    for (const auto& member : old_quorum) ++active_member_counts[member->proTxHash];
                }
                for (const auto& member : quorum) ++active_member_counts[member->proTxHash];
                const size_t repeated_members = std::accumulate(
                    active_member_counts.begin(), active_member_counts.end(), size_t{0},
                    [](size_t total, const auto& item) {
                        return total + (item.second > 1 ? item.second - 1 : 0);
                    });
                const size_t top_provider_overlap = std::count_if(
                    quorum.begin(), quorum.end(), [&](const auto& member) {
                        if (ProviderFor(seed, member->GetInternalId()) != 0) return false;
                        return std::any_of(
                            active_history.begin(), active_history.end(), [&](const auto& old_quorum) {
                                return std::any_of(
                                    old_quorum.begin(), old_quorum.end(), [&](const auto& old_member) {
                                        return old_member->proTxHash == member->proTxHash;
                                    });
                            });
                    });
                writer.WriteOverlap(
                    population,
                    profile,
                    round,
                    static_cast<double>(profile.size * profile.size) / population,
                    overlap,
                    active_history.size() + 1,
                    repeated_members,
                    MaxGroupMembers(quorum, [&](uint64_t member) { return ProviderFor(seed, member); }),
                    MaxGroupMembers(quorum, [&](uint64_t member) { return AsnFor(seed, member); }),
                    MaxGroupMembers(quorum, [&](uint64_t member) { return OperatorFor(seed, member); }),
                    MaxGroupMembers(quorum, [&](uint64_t member) { return CollateralOwnerFor(seed, member); }),
                    top_provider_overlap);

                for (const int offline_pct : OFFLINE_PCTS) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return !PercentEvent(seed, round, member->GetInternalId(), "offline", offline_pct);
                    });
                    writer.Write(MakeAvailabilityRow(
                        "independent_offline", population, profile, round, offline_pct,
                        quorum.size(), online, overlap));
                }

                for (const int failed_share : CORRELATED_GROUP_PCTS) {
                    const size_t failed_count =
                        (population * static_cast<size_t>(failed_share) + 99) / 100;
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return member->GetInternalId() >= failed_count;
                    });
                    writer.Write(MakeAvailabilityRow(
                        "largest_provider_failure", population, profile, round, failed_share,
                        quorum.size(), online, overlap));
                    writer.Write(MakeAvailabilityRow(
                        "largest_asn_failure", population, profile, round, failed_share,
                        quorum.size(), online, overlap));
                    writer.Write(MakeAvailabilityRow(
                        "largest_region_failure", population, profile, round, failed_share,
                        quorum.size(), online, overlap));
                }

                for (const int provider : {0, 1}) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return ProviderFor(seed, member->GetInternalId()) != provider;
                    });
                    writer.Write(MakeAvailabilityRow(
                        "provider_outage", population, profile, round, provider,
                        quorum.size(), online, overlap));
                }

                for (const int region : {0, 1}) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return RegionFor(seed, member->GetInternalId()) != region;
                    });
                    writer.Write(MakeAvailabilityRow(
                        "region_outage", population, profile, round, region,
                        quorum.size(), online, overlap));
                }

                for (const int provider_count : {2, 3}) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return ProviderFor(seed, member->GetInternalId()) >= provider_count;
                    });
                    writer.Write(MakeAvailabilityRow(
                        "multiple_provider_failure", population, profile, round, provider_count,
                        quorum.size(), online, overlap));
                }

                for (const int top_asn_count : {1, 3}) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return AsnFor(seed, member->GetInternalId()) >= top_asn_count;
                    });
                    writer.Write(MakeAvailabilityRow(
                        "asn_outage", population, profile, round, top_asn_count,
                        quorum.size(), online, overlap));
                }

                for (const int legacy_pct : MIXED_VERSION_PCTS) {
                    const size_t legacy_count =
                        (population * static_cast<size_t>(legacy_pct) + 99) / 100;
                    const size_t compatible = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return member->GetInternalId() >= legacy_count;
                    });
                    writer.Write(MakeAvailabilityRow(
                        "mixed_version", population, profile, round, legacy_pct,
                        quorum.size(), compatible, overlap));
                }

                for (const int concentration_pct : CONCENTRATION_PCTS) {
                    const size_t controlled_count =
                        (population * static_cast<size_t>(concentration_pct) + 99) / 100;
                    const size_t adversarial = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return member->GetInternalId() < controlled_count;
                    });
                    writer.Write(Row{
                        "operator_concentration",
                        population,
                        &profile,
                        round,
                        concentration_pct,
                        quorum.size(),
                        quorum.size(),
                        true,
                        true,
                        adversarial,
                        adversarial >= profile.threshold,
                        overlap,
                    });
                }

                for (const int concentration_pct : {10, 20, 33, 40}) {
                    const size_t controlled_count =
                        (population * static_cast<size_t>(concentration_pct) + 99) / 100;
                    const size_t controlled = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return member->GetInternalId() < controlled_count;
                    });
                    writer.Write(Row{
                        "collateral_owner_concentration",
                        population,
                        &profile,
                        round,
                        concentration_pct,
                        quorum.size(),
                        quorum.size(),
                        true,
                        true,
                        controlled,
                        controlled >= profile.threshold,
                        overlap,
                    });
                }

                // Markov-like deterministic flapping. A selected node is down for
                // three consecutive rounds after entering a down epoch.
                for (const int flap_pct : {5, 15, 30}) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        const uint64_t epoch = round / 3;
                        return !PercentEvent(seed, epoch, member->GetInternalId(), "flapping", flap_pct);
                    });
                    writer.Write(MakeAvailabilityRow(
                        "flapping", population, profile, round, flap_pct,
                        quorum.size(), online, overlap));
                }

                // A member is late when its deterministic latency percentile is
                // above the phase budget. Parameters express budget percentiles.
                for (const int accepted_percentile : {70, 80, 90, 95}) {
                    const size_t on_time = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return Random64(seed, round, member->GetInternalId(), "dkg-delay") % 100 <
                               static_cast<uint64_t>(accepted_percentile);
                    });
                    writer.Write(MakeAvailabilityRow(
                        "delayed_dkg_messages", population, profile, round, accepted_percentile,
                        quorum.size(), on_time, overlap));
                }

                for (const int restart_pct : {10, 20, 30}) {
                    const size_t online = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return !PercentEvent(
                            seed, round / 2, member->GetInternalId(), "restart-storm", restart_pct);
                    });
                    writer.Write(MakeAvailabilityRow(
                        "restart_storm", population, profile, round, restart_pct,
                        quorum.size(), online, overlap));
                }

                for (const int connected_pct : {50, 60, 70}) {
                    const size_t connected = std::count_if(quorum.begin(), quorum.end(), [&](const auto& member) {
                        return Random64(seed, 0, member->GetInternalId(), "partition-side") % 100 <
                               static_cast<uint64_t>(connected_pct);
                    });
                    writer.Write(MakeAvailabilityRow(
                        "partial_network_partition", population, profile, round, connected_pct,
                        quorum.size(), connected, overlap));
                }

                const size_t availability_online = std::count_if(
                    quorum.begin(), quorum.end(), [&](const auto& member) {
                        const int availability_class =
                            AvailabilityClassFor(seed, member->GetInternalId());
                        const int offline_pct = availability_class == 0 ? 1 :
                                                availability_class == 1 ? 5 : 30;
                        return !PercentEvent(
                            seed, round, member->GetInternalId(), "class-availability", offline_pct);
                    });
                writer.Write(MakeAvailabilityRow(
                    "availability_classes", population, profile, round, 0,
                    quorum.size(), availability_online, overlap));

                active_history.push_back(quorum);
                if (active_history.size() >= ACTIVE_QUORUM_WINDOW) {
                    active_history.erase(active_history.begin());
                }
                previous = quorum;
            }

            const auto [min_it, max_it] = std::minmax_element(selection_counts.begin(), selection_counts.end());
            const double expected = static_cast<double>(rounds * profile.size) / population;
            const double chi_square = std::accumulate(
                selection_counts.begin(), selection_counts.end(), 0.0,
                [expected](double sum, uint64_t observed) {
                    const double delta = static_cast<double>(observed) - expected;
                    return sum + delta * delta / expected;
                });
            const size_t mean_overlap = rounds > 1
                ? static_cast<size_t>(std::llround(overlap_sum / static_cast<double>(rounds - 1)))
                : 0;

            writer.Write(Row{
                "selection_summary",
                population,
                &profile,
                rounds,
                static_cast<int>(std::llround(chi_square)),
                profile.size,
                static_cast<size_t>(std::llround(expected)),
                true,
                true,
                *max_it - *min_it,
                false,
                mean_overlap,
            });
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
