// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/unbacked_headers.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>

BOOST_AUTO_TEST_SUITE(unbacked_headers_tests)

// The deep-fork warning's condition: headers the depth limit dropped from an outbound peer warrant it when they offer
// more work than the tip, or when their message was full under its own format's limit (GetHeadersLimit: 2000 plain
// headers, 8000 compressed ones), whatever their work.
BOOST_AUTO_TEST_CASE(deep_fork_warrants)
{
    using node::DeepForkWarrants;
    for (const size_t limit : {size_t{MAX_HEADERS_UNCOMPRESSED_RESULT}, size_t{MAX_HEADERS_COMPRESSED_RESULT}}) {
        BOOST_CHECK(DeepForkWarrants(/*more_work=*/true, /*count=*/1, limit));
        BOOST_CHECK(DeepForkWarrants(/*more_work=*/true, limit - 1, limit));
        BOOST_CHECK(DeepForkWarrants(/*more_work=*/false, limit, limit));
        BOOST_CHECK(!DeepForkWarrants(/*more_work=*/false, limit - 1, limit));
        BOOST_CHECK(!DeepForkWarrants(/*more_work=*/false, /*count=*/1, limit));
    }
    // A full plain message is not full under the compressed format's limit.
    BOOST_CHECK(!DeepForkWarrants(/*more_work=*/false, MAX_HEADERS_UNCOMPRESSED_RESULT, MAX_HEADERS_COMPRESSED_RESULT));
}

// The new branches inbound peers open are counted by network group (net_processing.cpp: 32 within 24 hours, at most
// 4096 groups). A group's count does not depend on which connection asks, so a reconnect does not start it again.
BOOST_AUTO_TEST_CASE(fork_budget_counts_by_group)
{
    using namespace std::chrono_literals;
    const std::chrono::seconds window{24h};
    node::UnbackedForkBudget budget{/*max_forks=*/32, window, /*max_groups=*/4096};
    std::array<CBlockIndex, 34> entries{}; // never connected
    const std::chrono::seconds t0{1'000'000};
    LOCK(cs_main); // whether an entry has been connected is read from the block index

    for (int i = 0; i < 32; ++i) {
        BOOST_CHECK(budget.Room(/*group=*/1, t0 + i * 1s));
        BOOST_CHECK(budget.Note(/*group=*/1, t0 + i * 1s, &entries[i]));
    }
    // The 33rd has no room for that group; another group has its own count.
    BOOST_CHECK(!budget.Room(1, t0 + 32s));
    BOOST_CHECK(budget.Room(2, t0 + 32s));
    BOOST_CHECK_EQUAL(budget.Groups(), 1U); // asking adds no group

    // A window after the first branch, and not a second before, there is room for one more, and only one.
    BOOST_CHECK(!budget.Room(1, t0 + window - 1s));
    BOOST_CHECK(budget.Room(1, t0 + window));
    BOOST_CHECK(budget.Note(1, t0 + window, &entries[32]));
    BOOST_CHECK(!budget.Room(1, t0 + window));

    // A branch whose first block has been connected no longer counts; one that failed still does.
    entries[5].nStatus = BLOCK_VALID_SCRIPTS;
    BOOST_CHECK(budget.Room(1, t0 + window));
    BOOST_CHECK(budget.Note(1, t0 + window, &entries[33]));
    entries[6].nStatus = BLOCK_VALID_SCRIPTS | BLOCK_FAILED_VALID;
    BOOST_CHECK(!budget.Room(1, t0 + window));
}

BOOST_AUTO_TEST_CASE(fork_budget_group_limit)
{
    using namespace std::chrono_literals;
    const std::chrono::seconds window{24h};
    node::UnbackedForkBudget budget{/*max_forks=*/32, window, /*max_groups=*/4096};
    std::array<CBlockIndex, 3> entries{};
    const std::chrono::seconds t0{1'000'000};
    LOCK(cs_main);

    // 4095 groups at t0, the 4096th an hour later: all held.
    for (uint64_t group = 0; group < 4095; ++group) {
        BOOST_REQUIRE(budget.Room(group, t0));
        BOOST_REQUIRE(budget.Note(group, t0, &entries[0]));
    }
    BOOST_CHECK(budget.Room(4095, t0 + 1h));
    BOOST_CHECK(budget.Note(4095, t0 + 1h, &entries[1]));
    BOOST_CHECK_EQUAL(budget.Groups(), 4096U);

    // Full, with nothing expired or connected: a group not held gets no room and is not noted, and no held group is
    // pushed out for it. A held group goes on with its own count.
    BOOST_CHECK(!budget.Room(5000, t0 + 2h));
    BOOST_CHECK(!budget.Note(5000, t0 + 2h, &entries[2]));
    BOOST_CHECK(!budget.Holds(5000));
    BOOST_CHECK(budget.Holds(7));
    BOOST_CHECK_EQUAL(budget.Groups(), 4096U);
    BOOST_CHECK(budget.Room(7, t0 + 2h));
    BOOST_CHECK(budget.Note(7, t0 + 2h, &entries[2]));
    BOOST_CHECK_EQUAL(budget.Groups(), 4096U);

    // A window after t0 the sweep empties the groups whose only branch has expired, and the new group gets in. Groups
    // 7 and 4095 have a newer branch each and stay.
    BOOST_CHECK(budget.Room(5000, t0 + window));
    BOOST_CHECK_EQUAL(budget.Groups(), 2U);
    BOOST_CHECK(budget.Note(5000, t0 + window, &entries[2]));
    BOOST_CHECK_EQUAL(budget.Groups(), 3U);

    // The sweep also drops connected branches: a table of two full groups takes a third once one branch is connected.
    node::UnbackedForkBudget small{/*max_forks=*/32, window, /*max_groups=*/2};
    std::array<CBlockIndex, 3> blocks{};
    BOOST_CHECK(small.Note(1, t0, &blocks[0]));
    BOOST_CHECK(small.Note(2, t0, &blocks[1]));
    BOOST_CHECK(!small.Room(3, t0 + 1s));
    blocks[0].nStatus = BLOCK_VALID_SCRIPTS;
    BOOST_CHECK(small.Room(3, t0 + 1s));
    BOOST_CHECK_EQUAL(small.Groups(), 1U);
    BOOST_CHECK(small.Note(3, t0 + 1s, &blocks[2]));
    BOOST_CHECK(!small.Room(1, t0 + 2s)); // group 1 was emptied and dropped: now it is the one not held
}

BOOST_AUTO_TEST_SUITE_END()
