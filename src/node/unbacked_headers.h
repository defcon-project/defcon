// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UNBACKED_HEADERS_H
#define BITCOIN_NODE_UNBACKED_HEADERS_H

#include <chain.h>
#include <sync.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>

namespace node {
/**
 * The new branches of headers without their blocks (or of blocks not asked for) that inbound peers opened, counted by
 * the peer's network group instead of by its connection: a peer that reconnects, or opens several connections, keeps
 * drawing on the one budget of its group (net_processing.cpp, MAX_UNBACKED_FORKS_PER_PEER).
 *
 * Each group may have `max_forks` branches noted within `window` whose first entry was never connected; older ones and
 * connected ones are forgotten whenever the group is looked at. At most `max_groups` groups are held: when a group not
 * held yet asks and the table is full, every group is swept once (expired and connected entries, then empty groups),
 * and if the table is still full the new group gets no room. A group already held is never pushed out, so the entries
 * are bounded by max_groups * max_forks. The entries point into the block index, and whether one has been connected
 * is read from it: every call that looks at them needs cs_main, which also guards the table.
 */
class UnbackedForkBudget
{
public:
    using Key = uint64_t;

    UnbackedForkBudget(size_t max_forks, std::chrono::seconds window, size_t max_groups)
        : m_max_forks{max_forks}, m_window{window}, m_max_groups{max_groups} {}

    /** Whether the group may open another new branch now. */
    bool Room(Key group, std::chrono::seconds now) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        const auto it{m_groups.find(group)};
        if (it != m_groups.end()) {
            Prune(it->second, now);
            return it->second.size() < m_max_forks;
        }
        return MakeRoomForGroup(now);
    }

    /** Notes that the group opened a new branch whose first entry is `first`. Returns false, noting nothing, only when
     *  the group is not held and the table stays full after a sweep (the groups filled up after Room said yes). */
    bool Note(Key group, std::chrono::seconds now, const CBlockIndex* first) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        auto it{m_groups.find(group)};
        if (it == m_groups.end()) {
            if (!MakeRoomForGroup(now)) return false;
            it = m_groups.emplace(group, Forks{}).first;
        }
        it->second.emplace_back(now, first);
        return true;
    }

    /** Whether the group is held (it has, or had until the last look at it, a branch noted). */
    bool Holds(Key group) const { return m_groups.count(group) != 0; }

    size_t Groups() const { return m_groups.size(); }

private:
    using Forks = std::deque<std::pair<std::chrono::seconds, const CBlockIndex*>>;

    void Prune(Forks& forks, std::chrono::seconds now) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        for (auto it{forks.begin()}; it != forks.end();) {
            if (it->first + m_window <= now || it->second->IsValid(BLOCK_VALID_SCRIPTS)) {
                it = forks.erase(it);
            } else {
                ++it;
            }
        }
    }

    /** Whether a group not held yet can be added, sweeping every group once if the table is full. */
    bool MakeRoomForGroup(std::chrono::seconds now) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        if (m_groups.size() < m_max_groups) return true;
        for (auto it{m_groups.begin()}; it != m_groups.end();) {
            Prune(it->second, now);
            if (it->second.empty()) {
                it = m_groups.erase(it);
            } else {
                ++it;
            }
        }
        return m_groups.size() < m_max_groups;
    }

    const size_t m_max_forks;
    const std::chrono::seconds m_window;
    const size_t m_max_groups;
    std::map<Key, Forks> m_groups;
};

/**
 * Whether headers the depth limit on headers without their blocks dropped from an outbound peer warrant the node's
 * deep-fork warning (net_processing.cpp, MaybeWarnDeepFork): they offer more work than the active tip, or the message
 * held as many headers as its own format allows (`limit`: 2000 plain headers, 8000 compressed ones, see
 * GetHeadersLimit), so the peer has more to send. Anything else may be a short, lower-work branch of no interest.
 */
constexpr bool DeepForkWarrants(bool more_work, size_t count, size_t limit)
{
    return more_work || count >= limit;
}
} // namespace node

#endif // BITCOIN_NODE_UNBACKED_HEADERS_H
