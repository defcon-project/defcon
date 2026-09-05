// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license.

/**
 * Specification scaffold only.
 *
 * Production Core does not yet expose a signed-height ChainLock profile
 * resolver. These tests define the required boundary and fail-safe behavior
 * without adding a mainnet activation height or changing chainparams.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace {

enum class TestChainLockProfile {
    LEGACY,
    Q60_V2,
};

struct TestProfileSchedule {
    std::optional<int32_t> activation_height;
    bool legacy_type_available{true};
    bool q60_v2_type_available{false};

    void Validate() const
    {
        if (activation_height.has_value() && *activation_height < 1) {
            throw std::invalid_argument("activation height must be positive");
        }
        if (!legacy_type_available) {
            throw std::invalid_argument("legacy ChainLock type is unavailable");
        }
        if (activation_height.has_value() && !q60_v2_type_available) {
            throw std::invalid_argument("Q60 V2 type must be available before activation");
        }
    }
};

TestChainLockProfile GetChainLockQuorumTypeForTest(
    int32_t signed_height,
    const TestProfileSchedule& schedule)
{
    schedule.Validate();
    if (signed_height < 0) {
        throw std::invalid_argument("signed height must be non-negative");
    }
    if (!schedule.activation_height.has_value() ||
        signed_height < *schedule.activation_height) {
        return TestChainLockProfile::LEGACY;
    }
    return TestChainLockProfile::Q60_V2;
}

} // namespace

BOOST_AUTO_TEST_SUITE(chainlock_profile_resolver_tests)

BOOST_AUTO_TEST_CASE(no_activation_is_legacy_only)
{
    const TestProfileSchedule schedule{};
    BOOST_CHECK(GetChainLockQuorumTypeForTest(0, schedule) ==
                TestChainLockProfile::LEGACY);
    BOOST_CHECK(GetChainLockQuorumTypeForTest(1000000, schedule) ==
                TestChainLockProfile::LEGACY);
}

BOOST_AUTO_TEST_CASE(signed_height_boundary_is_exact)
{
    const TestProfileSchedule schedule{1000, true, true};
    BOOST_CHECK(GetChainLockQuorumTypeForTest(999, schedule) ==
                TestChainLockProfile::LEGACY);
    BOOST_CHECK(GetChainLockQuorumTypeForTest(1000, schedule) ==
                TestChainLockProfile::Q60_V2);
    BOOST_CHECK(GetChainLockQuorumTypeForTest(1001, schedule) ==
                TestChainLockProfile::Q60_V2);
}

BOOST_AUTO_TEST_CASE(historical_resolution_is_stable_after_reconstruction)
{
    const TestProfileSchedule before_restart{500, true, true};
    const auto historical =
        GetChainLockQuorumTypeForTest(499, before_restart);

    // Reconstructing the same consensus schedule models restart/reindex. The
    // result depends only on signed height and immutable schedule data.
    const TestProfileSchedule after_restart_or_reindex{500, true, true};
    BOOST_CHECK(
        GetChainLockQuorumTypeForTest(499, after_restart_or_reindex) ==
        historical);
    BOOST_CHECK(
        GetChainLockQuorumTypeForTest(500, after_restart_or_reindex) ==
        TestChainLockProfile::Q60_V2);
}

BOOST_AUTO_TEST_CASE(invalid_configuration_fails_closed)
{
    BOOST_CHECK_THROW(
        GetChainLockQuorumTypeForTest(1, TestProfileSchedule{0, true, true}),
        std::invalid_argument);
    BOOST_CHECK_THROW(
        GetChainLockQuorumTypeForTest(1000, TestProfileSchedule{1000, true, false}),
        std::invalid_argument);
    BOOST_CHECK_THROW(
        GetChainLockQuorumTypeForTest(1, TestProfileSchedule{std::nullopt, false, false}),
        std::invalid_argument);
    BOOST_CHECK_THROW(
        GetChainLockQuorumTypeForTest(-1, TestProfileSchedule{}),
        std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
