// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license.

/**
 * Specification scaffold only.
 *
 * Production Core does not yet persist both valid competing CLSIG evidences or
 * expose FINALITY_CONFLICT. This isolated state model defines testable desired
 * behavior without changing production consensus or chain selection.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

namespace {

enum class TestFinalityState {
    NORMAL,
    FINALITY_CONFLICT,
};

struct TestEvidence {
    int32_t height;
    std::string block_hash;
    std::string signature;
    bool valid;

    auto Key() const { return std::tie(height, block_hash, signature); }
};

class TestFinalityConflictStore {
private:
    TestFinalityState state{TestFinalityState::NORMAL};
    std::map<int32_t, std::set<std::pair<std::string, std::string>>> evidence;
    size_t invalid_count{0};

    void RecomputeState()
    {
        state = TestFinalityState::NORMAL;
        for (const auto& [_, items] : evidence) {
            std::set<std::string> block_hashes;
            for (const auto& item : items) block_hashes.insert(item.first);
            if (block_hashes.size() > 1) {
                state = TestFinalityState::FINALITY_CONFLICT;
                return;
            }
        }
    }

public:
    bool Add(const TestEvidence& item)
    {
        if (!item.valid) {
            ++invalid_count;
            return false;
        }
        const bool inserted =
            evidence[item.height].emplace(item.block_hash, item.signature).second;
        RecomputeState();
        return inserted;
    }

    [[nodiscard]] TestFinalityState State() const { return state; }
    [[nodiscard]] bool SigningAllowed() const
    {
        return state != TestFinalityState::FINALITY_CONFLICT;
    }
    [[nodiscard]] size_t InvalidCount() const { return invalid_count; }
    [[nodiscard]] size_t EvidenceCount(int32_t height) const
    {
        const auto it = evidence.find(height);
        return it == evidence.end() ? 0 : it->second.size();
    }

    [[nodiscard]] std::string Persist() const
    {
        std::ostringstream stream;
        for (const auto& [height, items] : evidence) {
            for (const auto& [hash, signature] : items) {
                stream << height << ' ' << hash << ' ' << signature << '\n';
            }
        }
        return stream.str();
    }

    static TestFinalityConflictStore Reload(const std::string& persisted)
    {
        TestFinalityConflictStore result;
        std::istringstream stream{persisted};
        TestEvidence item{};
        item.valid = true;
        while (stream >> item.height >> item.block_hash >> item.signature) {
            result.Add(item);
        }
        return result;
    }
};

TestEvidence Evidence(int32_t height, std::string hash, std::string signature)
{
    return TestEvidence{height, std::move(hash), std::move(signature), true};
}

} // namespace

BOOST_AUTO_TEST_SUITE(chainlock_finality_conflict_model_tests)

BOOST_AUTO_TEST_CASE(competing_valid_clsigs_enter_safe_state)
{
    TestFinalityConflictStore store;
    BOOST_CHECK(store.Add(Evidence(100, "block-a", "sig-a")));
    BOOST_CHECK(store.SigningAllowed());
    BOOST_CHECK(store.Add(Evidence(100, "block-b", "sig-b")));
    BOOST_CHECK(store.State() == TestFinalityState::FINALITY_CONFLICT);
    BOOST_CHECK(!store.SigningAllowed());
    BOOST_CHECK_EQUAL(store.EvidenceCount(100), 2);
}

BOOST_AUTO_TEST_CASE(arrival_order_peer_count_and_chain_length_do_not_choose_winner)
{
    TestFinalityConflictStore first;
    first.Add(Evidence(100, "block-a", "sig-a"));
    first.Add(Evidence(100, "block-b", "sig-b"));

    TestFinalityConflictStore reversed;
    reversed.Add(Evidence(100, "block-b", "sig-b"));
    reversed.Add(Evidence(100, "block-a", "sig-a"));

    BOOST_CHECK(first.State() == reversed.State());
    BOOST_CHECK_EQUAL(first.Persist(), reversed.Persist());
}

BOOST_AUTO_TEST_CASE(delayed_and_duplicate_clsigs_are_idempotent)
{
    TestFinalityConflictStore store;
    const auto first = Evidence(100, "block-a", "sig-a");
    BOOST_CHECK(store.Add(first));
    BOOST_CHECK(!store.Add(first));
    BOOST_CHECK_EQUAL(store.EvidenceCount(100), 1);

    BOOST_CHECK(store.Add(Evidence(100, "block-b", "sig-b")));
    BOOST_CHECK(!store.Add(Evidence(100, "block-b", "sig-b")));
    BOOST_CHECK_EQUAL(store.EvidenceCount(100), 2);
    BOOST_CHECK(!store.SigningAllowed());
}

BOOST_AUTO_TEST_CASE(invalid_flood_does_not_create_conflict)
{
    TestFinalityConflictStore store;
    for (int i = 0; i < 1000; ++i) {
        BOOST_CHECK(!store.Add(TestEvidence{
            100, "invalid-" + std::to_string(i), "bad", false}));
    }
    BOOST_CHECK(store.State() == TestFinalityState::NORMAL);
    BOOST_CHECK(store.SigningAllowed());
    BOOST_CHECK_EQUAL(store.InvalidCount(), 1000);
}

BOOST_AUTO_TEST_CASE(restart_and_reindex_reload_both_evidences)
{
    TestFinalityConflictStore store;
    store.Add(Evidence(100, "block-a", "sig-a"));
    store.Add(Evidence(100, "block-b", "sig-b"));
    const std::string persisted = store.Persist();

    const auto after_restart = TestFinalityConflictStore::Reload(persisted);
    const auto after_reindex = TestFinalityConflictStore::Reload(persisted);
    BOOST_CHECK(after_restart.State() == TestFinalityState::FINALITY_CONFLICT);
    BOOST_CHECK(after_reindex.State() == TestFinalityState::FINALITY_CONFLICT);
    BOOST_CHECK_EQUAL(after_restart.EvidenceCount(100), 2);
    BOOST_CHECK(!after_restart.SigningAllowed());
}

BOOST_AUTO_TEST_CASE(non_conflicting_higher_chainlock_does_not_clear_conflict)
{
    TestFinalityConflictStore store;
    store.Add(Evidence(100, "block-a", "sig-a"));
    store.Add(Evidence(100, "block-b", "sig-b"));
    store.Add(Evidence(101, "block-c", "sig-c"));
    BOOST_CHECK(store.State() == TestFinalityState::FINALITY_CONFLICT);
    BOOST_CHECK(!store.SigningAllowed());
}

BOOST_AUTO_TEST_SUITE_END()
