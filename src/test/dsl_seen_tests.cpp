// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_seen.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using dsl::SeenTable;
using PeerId = SeenTable::PeerId;

namespace {
uint256 K(const std::string& type, const std::string& bytes)
{
    return SeenTable::Key(type, MakeUCharSpan(bytes));
}
using Peers = std::vector<PeerId>;
} // namespace

BOOST_FIXTURE_TEST_SUITE(dsl_seen_tests, BasicTestingSetup)

// sentinelfix.md 4.3/2-3: a bit is set by the bytes a peer sent us or we sent it, and the relay leaves out exactly
// the peers whose bit is set.
BOOST_AUTO_TEST_CASE(a_peer_that_has_the_bytes_is_a_holder_and_no_other_is)
{
    SeenTable t;
    t.SetCapacity(100);
    for (const PeerId p : {1, 2, 3, 4}) t.AddPeer(p);
    const uint256 key{K("posereport", "abc")};
    BOOST_CHECK(t.Holders(key).empty());
    t.NoteReceived(key, 10, 2, t.Generation());
    BOOST_CHECK(t.Holders(key) == Peers({2}));
    t.NoteSent(key, 10, {3, 1}, t.Generation());
    BOOST_CHECK(t.Holders(key) == Peers({1, 2, 3}));
    t.NoteReceived(key, 10, 2, t.Generation());
    BOOST_CHECK(t.Holders(key) == Peers({1, 2, 3}));
    BOOST_CHECK_EQUAL(t.Size(), 1U);
}

// 4.3/4: only exactly equal bytes of the same type. Another byte, another length or another type is another message.
BOOST_AUTO_TEST_CASE(only_the_exact_bytes_of_the_same_type_count)
{
    SeenTable t;
    t.SetCapacity(100);
    t.AddPeer(7);
    t.NoteReceived(K("posereport", "abc"), 10, 7, t.Generation());
    BOOST_CHECK(t.Holders(K("posereport", "abc")) == Peers({7}));
    BOOST_CHECK(t.Holders(K("posereport", "abd")).empty());
    BOOST_CHECK(t.Holders(K("posereport", std::string("abc\0", 4))).empty());
    BOOST_CHECK(t.Holders(K("posereport", "ab")).empty());
    BOOST_CHECK(t.Holders(K("poseresp", "abc")).empty());
    BOOST_CHECK(t.Holders(K("posecommit", "abc")).empty());
    // the separator: moving bytes between the type and the payload makes another key
    BOOST_CHECK(K("poseresp", "abc") != K("poseres", "pabc"));
}

// 4.3/1: a disconnected peer's bit leaves every entry before its slot is handed out again.
BOOST_AUTO_TEST_CASE(a_new_peer_never_inherits_a_disconnected_peer_s_bits)
{
    SeenTable t;
    t.SetCapacity(100);
    t.AddPeer(1);
    t.AddPeer(2);
    const uint256 a{K("posereport", "a")};
    const uint256 b{K("poseresp", "b")};
    t.NoteReceived(a, 10, 1, t.Generation());
    t.NoteSent(b, 10, {1, 2}, t.Generation());
    t.RemovePeer(1);
    BOOST_CHECK(t.Holders(a).empty());
    BOOST_CHECK(t.Holders(b) == Peers({2}));
    t.AddPeer(3); // the free slot, the one peer 1 had
    BOOST_CHECK(t.Holders(a).empty());
    BOOST_CHECK(t.Holders(b) == Peers({2}));
    // a send noted for the departed peer after it left marks nobody, the new holder of its slot included
    t.NoteSent(a, 10, {1}, t.Generation());
    BOOST_CHECK(t.Holders(a).empty());
    t.NoteReceived(a, 10, 3, t.Generation());
    BOOST_CHECK(t.Holders(a) == Peers({3}));
    // a peer that never connected is never a holder
    t.NoteReceived(b, 10, 42, t.Generation());
    BOOST_CHECK(t.Holders(b) == Peers({2}));
}

// The table follows the store: every relay is of bytes the store has just taken as new, so a second relay of the same
// bytes means the store forgot them -- and the peers may have too, after a rewind back across the epoch's end that
// keeps its base, or a reorg away from and back onto the same base. The relay marks the entry, and a marked entry
// starts over at the next relay: nobody is left out. Its bits are kept, as 4.3/2 asks, but no relay reads them.
BOOST_AUTO_TEST_CASE(a_second_relay_of_the_same_bytes_starts_over)
{
    SeenTable t;
    t.SetCapacity(100);
    for (const PeerId p : {1, 2, 3}) t.AddPeer(p);
    const uint256 key{K("posereport", "r")};
    // received from 1 before it could be relayed: the first relay leaves 1 out
    t.NoteReceived(key, 10, 1, t.Generation());
    BOOST_CHECK(t.BeginRelay(key) == Peers({1}));
    t.NoteRelayed(key, 10, {2, 3});
    BOOST_CHECK(t.Holders(key) == Peers({1, 2, 3}));
    // the same bytes relayed again: the store took them anew, so everyone is sent them again
    BOOST_CHECK(t.BeginRelay(key).empty());
    BOOST_CHECK(t.Holders(key).empty());
    t.NoteRelayed(key, 10, {1, 3});
    BOOST_CHECK(t.Holders(key) == Peers({1, 3}));
    // a copy received after that is noted, but no later relay reads it: the next one starts over as well
    t.NoteReceived(key, 10, 2, t.Generation());
    BOOST_CHECK(t.Holders(key) == Peers({1, 2, 3}));
    BOOST_CHECK(t.BeginRelay(key).empty());
    // a send outside a relay (an answer to a request) does not mark the entry: the next relay is a first one
    const uint256 other{K("poseresp", "a")};
    t.NoteSent(other, 10, {2}, t.Generation());
    BOOST_CHECK(t.BeginRelay(other) == Peers({2}));
    // a relay that reached nobody still marks its entry, if there is room: a copy received after it does not make
    // the next relay a first one
    const uint256 lone{K("posereport", "lone")};
    t.NoteRelayed(lone, 10, {});
    t.NoteReceived(lone, 10, 2, t.Generation());
    BOOST_CHECK(t.Holders(lone) == Peers({2}));
    BOOST_CHECK(t.BeginRelay(lone).empty());
    // with no room the relay makes no entry and no mark, and a later copy makes the entry unmarked (pose_service_seen.h)
    t.SetCapacity(t.Size());
    const uint256 full{K("posereport", "full")};
    t.NoteRelayed(full, 10, {1});
    BOOST_CHECK_EQUAL(t.Size(), 3U);
    t.SetCapacity(100);
    t.NoteReceived(full, 10, 3, t.Generation());
    BOOST_CHECK(t.BeginRelay(full) == Peers({3}));
}

// More connections than one word of slots.
BOOST_AUTO_TEST_CASE(slots_beyond_the_first_word)
{
    SeenTable t;
    t.SetCapacity(100);
    for (PeerId p = 0; p < 200; ++p) t.AddPeer(p);
    const uint256 key{K("posereport", "x")};
    t.NoteSent(key, 10, {0, 63, 64, 127, 128, 199}, t.Generation());
    BOOST_CHECK(t.Holders(key) == Peers({0, 63, 64, 127, 128, 199}));
    t.RemovePeer(64);
    BOOST_CHECK(t.Holders(key) == Peers({0, 63, 127, 128, 199}));
    t.AddPeer(500); // slot 64 again
    BOOST_CHECK(t.Holders(key) == Peers({0, 63, 127, 128, 199}));
}

// 6.1: the message gets through a single connection with a full table and with an expired one.
BOOST_AUTO_TEST_CASE(a_full_or_expired_table_still_lets_a_message_through_a_single_connection)
{
    SeenTable t;
    t.SetCapacity(4);
    t.AddPeer(9); // the only connection
    for (int i = 0; i < 4; ++i) t.NoteReceived(K("posereport", ToString(i)), 10, 9, t.Generation());
    BOOST_CHECK_EQUAL(t.Size(), 4U);
    // a message the full table has never seen is held by nobody, so nothing withholds it from the one peer
    const uint256 fresh{K("posereport", "fresh")};
    BOOST_CHECK(t.Holders(fresh).empty());
    // one older than everything in the full table, or of the oldest epoch in it, is not entered: it is never
    // withheld either
    t.NoteReceived(K("posereport", "old"), 9, 9, t.Generation());
    BOOST_CHECK(t.Holders(K("posereport", "old")).empty());
    BOOST_CHECK_EQUAL(t.Size(), 4U);
    // a newer one makes room by dropping the oldest epoch: those messages would be sent again, never withheld
    t.NoteReceived(fresh, 11, 9, t.Generation());
    BOOST_CHECK(t.Holders(fresh) == Peers({9}));
    BOOST_CHECK_EQUAL(t.Size(), 1U);
    BOOST_CHECK(t.Holders(K("posereport", "0")).empty());
    // expiry keeps the tip's epoch and the one before
    t.NoteReceived(K("posereport", "e12"), 12, 9, t.Generation());
    t.Expire(13);
    BOOST_CHECK(t.Holders(fresh).empty());
    BOOST_CHECK(t.Holders(K("posereport", "e12")) == Peers({9}));
    t.Expire(14);
    BOOST_CHECK_EQUAL(t.Size(), 0U);
    // with no room at all nothing is ever entered, and so nothing is ever withheld
    t.SetCapacity(0);
    t.NoteReceived(fresh, 14, 9, t.Generation());
    t.NoteSent(fresh, 14, {9}, t.Generation());
    BOOST_CHECK(t.Holders(fresh).empty());
    BOOST_CHECK_EQUAL(t.Size(), 0U);
}

// 4.3/5 after a capacity cut: a lower capacity drops nothing by itself; a new entry makes room only by dropping whole
// epochs older than its own, the oldest first, and is not made if that does not make room.
BOOST_AUTO_TEST_CASE(a_lower_capacity_binds_from_the_next_new_entry)
{
    SeenTable t;
    t.SetCapacity(6);
    t.AddPeer(1);
    const std::vector<std::pair<std::string, uint32_t>> held{{"a", 9}, {"b", 10}, {"c", 10}, {"d", 11}, {"e", 11}, {"f", 11}};
    for (const auto& [name, epoch] : held) t.NoteReceived(K("posereport", name), epoch, 1, t.Generation());
    BOOST_CHECK_EQUAL(t.Size(), 6U);
    t.SetCapacity(3);
    BOOST_CHECK_EQUAL(t.Size(), 6U);
    // one more of epoch 11 drops epochs 9 and 10, never its own, and still does not fit
    t.NoteReceived(K("posereport", "g"), 11, 1, t.Generation());
    BOOST_CHECK_EQUAL(t.Size(), 3U);
    BOOST_CHECK(t.Holders(K("posereport", "g")).empty());
    for (const char* name : {"a", "b", "c"}) BOOST_CHECK(t.Holders(K("posereport", name)).empty());
    for (const char* name : {"d", "e", "f"}) BOOST_CHECK(t.Holders(K("posereport", name)) == Peers({1}));
    // one of epoch 12 drops epoch 11 whole and gets in
    t.NoteReceived(K("posereport", "h"), 12, 1, t.Generation());
    BOOST_CHECK_EQUAL(t.Size(), 1U);
    BOOST_CHECK(t.Holders(K("posereport", "h")) == Peers({1}));
    BOOST_CHECK(t.Holders(K("posereport", "d")).empty());
}

// What the store forgets in a reorg the table forgets too: ForgetFrom() drops the entries of an epoch and of every
// later one, relayed or not, and keeps the older ones; the epoch index follows.
BOOST_AUTO_TEST_CASE(forgetting_from_an_epoch_drops_it_and_every_later_one)
{
    SeenTable t;
    t.SetCapacity(100);
    t.AddPeer(1);
    for (const auto& [name, epoch] : std::vector<std::pair<std::string, uint32_t>>{{"a", 9}, {"b", 10}, {"c", 11}}) {
        t.NoteReceived(K("posereport", name), epoch, 1, t.Generation());
    }
    t.NoteRelayed(K("posereport", "c"), 11, {});
    t.ForgetFrom(10);
    BOOST_CHECK_EQUAL(t.Size(), 1U);
    BOOST_CHECK(t.Holders(K("posereport", "a")) == Peers({1}));
    BOOST_CHECK(t.Holders(K("posereport", "b")).empty());
    // a forgotten relayed entry goes with its mark: the same bytes, noted again, make a first relay
    t.NoteReceived(K("posereport", "c"), 11, 1, t.Generation());
    BOOST_CHECK(t.BeginRelay(K("posereport", "c")) == Peers({1}));
    // and a full table still makes room from the epochs that are left
    t.SetCapacity(2);
    t.NoteReceived(K("posereport", "d"), 12, 1, t.Generation());
    BOOST_CHECK_EQUAL(t.Size(), 2U);
    BOOST_CHECK(t.Holders(K("posereport", "d")) == Peers({1}));
    BOOST_CHECK(t.Holders(K("posereport", "a")).empty());
}

// Astra r1 R1: following a reorg, ordered against the thread that notes. The message thread reads the generation
// before it looks at the store, sees the store hold the bytes, and stops before its note; the store forgets the epoch
// in a reorg and the table follows (ForgetFrom); then the note lands. It is dropped: a copy seen before the forgetting
// holds back nothing when the store takes the bytes again. NoteSent likewise; a note read after the forget lands.
BOOST_AUTO_TEST_CASE(a_note_read_before_a_forget_is_dropped)
{
    SeenTable t;
    t.SetCapacity(100);
    t.AddPeer(1);
    t.AddPeer(2);
    const uint256 r{K("posereport", "r")};
    t.NoteRelayed(r, 10, {1, 2}); // taken and relayed: marked
    const uint64_t seen{t.Generation()}; // peer 1's copy: the store holds it ... and the thread stops here
    {
        const SeenTable::ForgetWindow window{t};
        t.ForgetFrom(10);
    }
    BOOST_CHECK_EQUAL(t.Size(), 0U);
    t.NoteReceived(r, 10, 1, seen);
    BOOST_CHECK_EQUAL(t.Size(), 0U);
    BOOST_CHECK(t.BeginRelay(r).empty()); // the store takes r again: nobody is left out
    const uint64_t sent{t.Generation()};
    t.ForgetFrom(10);
    t.NoteSent(r, 10, {2}, sent);
    BOOST_CHECK_EQUAL(t.Size(), 0U);
    t.NoteReceived(r, 10, 1, t.Generation());
    BOOST_CHECK(t.Holders(r) == Peers({1}));
    BOOST_CHECK(t.BeginRelay(r) == Peers({1}));
}

// The rest of R1: between the store's forgetting and the table's, a relay names nobody and a note is dropped -- the
// ForgetWindow around both. A window in which nothing was forgotten leaves the table as it was, and a note read before
// it still lands.
BOOST_AUTO_TEST_CASE(a_forget_window_withholds_nothing_and_takes_no_note)
{
    SeenTable t;
    t.SetCapacity(100);
    t.AddPeer(1);
    t.AddPeer(2);
    const uint256 r{K("posereport", "r")};
    const uint256 s{K("posereport", "s")};
    t.NoteReceived(r, 10, 1, t.Generation());
    BOOST_REQUIRE(t.Holders(r) == Peers({1}));
    const uint64_t before{t.Generation()};
    {
        const SeenTable::ForgetWindow window{t};
        BOOST_CHECK(t.BeginRelay(r).empty());
        t.NoteReceived(s, 10, 2, t.Generation());
        t.NoteSent(s, 10, {1}, t.Generation());
        BOOST_CHECK(t.Holders(s).empty());
    }
    BOOST_CHECK(t.BeginRelay(r) == Peers({1}));
    t.NoteReceived(s, 10, 2, before);
    BOOST_CHECK(t.Holders(s) == Peers({2}));
}

// 4.3/4 as a property: whatever the order of connections, disconnections, receipts, sends, relays, expiries,
// forgetting and capacity changes, Holders() and BeginRelay() name only connected peers that received or were sent
// exactly these bytes on their current connection -- so the table can withhold a send only from a peer that has the
// message. The model is keyed by the message (its index), not by Key(), so a key that matched other bytes shows.
BOOST_AUTO_TEST_CASE(holders_are_always_peers_that_have_the_bytes)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    SeenTable t;
    t.SetCapacity(8);
    std::set<PeerId> connected;
    std::map<size_t, std::set<PeerId>> have; // per message index, the peers that have it on their current connection
    std::vector<uint256> keys;
    for (int i = 0; i < 24; ++i) keys.push_back(K(i % 2 ? "posereport" : "poseresp", ToString(i)));
    PeerId next_peer{0};
    uint32_t tip_epoch{10};
    size_t withheld_checks{0};
    size_t capacity{8};
    size_t max_capacity{8};
    const auto any_connected = [&]() {
        auto it = connected.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng.randrange(connected.size())));
        return *it;
    };
    for (int step = 0; step < 6000; ++step) {
        const size_t size_before{t.Size()};
        const size_t ki{static_cast<size_t>(rng.randrange(keys.size()))};
        const uint256& key = keys[ki];
        const uint32_t epoch{tip_epoch - 1 + static_cast<uint32_t>(rng.randrange(3))};
        switch (rng.randrange(10)) {
        case 0:
            t.AddPeer(next_peer);
            connected.insert(next_peer++);
            break;
        case 1:
            if (!connected.empty()) {
                const PeerId p{any_connected()};
                t.RemovePeer(p);
                connected.erase(p);
                for (auto& [k, peers] : have) peers.erase(p);
            }
            break;
        case 2:
        case 3:
            if (!connected.empty()) {
                const PeerId p{any_connected()};
                t.NoteReceived(key, epoch, p, t.Generation());
                have[ki].insert(p);
            }
            break;
        case 4:
        case 5: {
            Peers to;
            for (const PeerId p : connected) {
                if (rng.randbool()) to.push_back(p);
            }
            if (rng.randrange(4) == 0) to.push_back(next_peer + 1000); // not connected
            t.NoteSent(key, epoch, to, t.Generation());
            for (const PeerId p : to) {
                if (connected.count(p) != 0) have[ki].insert(p);
            }
            break;
        }
        case 6:
            if (rng.randrange(10) == 0) t.Expire(++tip_epoch);
            break;
        case 7:
            if (rng.randrange(25) == 0) {
                capacity = static_cast<size_t>(rng.randrange(12));
                t.SetCapacity(capacity);
                max_capacity = std::max(max_capacity, capacity);
            }
            break;
        case 8: { // a relay: leaves out what BeginRelay names, sends to some of the rest, notes them
            const Peers holders{t.BeginRelay(key)};
            for (const PeerId p : holders) {
                ++withheld_checks;
                BOOST_REQUIRE_MESSAGE(connected.count(p) != 0 && have[ki].count(p) != 0,
                                      "step " << step << ": the relay would leave out peer " << p << " without the bytes");
            }
            Peers to;
            for (const PeerId p : connected) {
                if (!std::binary_search(holders.begin(), holders.end(), p) && rng.randbool()) to.push_back(p);
            }
            t.NoteRelayed(key, epoch, to);
            for (const PeerId p : to) have[ki].insert(p);
            break;
        }
        case 9:
            if (rng.randrange(20) == 0) t.ForgetFrom(epoch);
            break;
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            for (const PeerId p : t.Holders(keys[i])) {
                ++withheld_checks;
                BOOST_REQUIRE_MESSAGE(connected.count(p) != 0 && have[i].count(p) != 0,
                                      "step " << step << ": peer " << p << " named a holder without the bytes");
            }
        }
        // a table that grows never exceeds the capacity of the moment (the eviction path is checked by
        // a_lower_capacity_binds_from_the_next_new_entry), and it never outgrows the largest capacity
        if (t.Size() > size_before) BOOST_REQUIRE_LE(t.Size(), capacity);
        BOOST_REQUIRE_LE(t.Size(), max_capacity);
    }
    // the property was exercised, not vacuous
    BOOST_CHECK_GT(withheld_checks, 1000U);
}

BOOST_AUTO_TEST_SUITE_END()
