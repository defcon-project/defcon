// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_seen.h>

#include <crypto/sha256.h>

#include <algorithm>
#include <iterator>

namespace dsl {

uint256 SeenTable::Key(const std::string& msg_type, Span<const unsigned char> payload)
{
    // The type, a zero byte (no message type contains one), then the payload.
    static constexpr unsigned char SEPARATOR{0};
    uint256 key;
    CSHA256()
        .Write(reinterpret_cast<const unsigned char*>(msg_type.data()), msg_type.size())
        .Write(&SEPARATOR, 1)
        .Write(payload.data(), payload.size())
        .Finalize(key.begin());
    return key;
}

void SeenTable::AddPeer(PeerId peer)
{
    LOCK(m_mutex);
    if (peer < 0 || m_slot_of.count(peer) != 0) return;
    const auto free_slot = std::find(m_owner.begin(), m_owner.end(), NO_PEER);
    const size_t slot = static_cast<size_t>(std::distance(m_owner.begin(), free_slot));
    if (free_slot == m_owner.end()) {
        m_owner.push_back(peer);
        m_slot_dirty.push_back(false);
    } else {
        *free_slot = peer;
    }
    m_slot_of.emplace(peer, slot);
}

void SeenTable::RemovePeer(PeerId peer)
{
    LOCK(m_mutex);
    const auto it = m_slot_of.find(peer);
    if (it == m_slot_of.end()) return;
    const size_t slot = it->second;
    const size_t word = slot / 64;
    const uint64_t keep = ~(uint64_t{1} << (slot % 64));
    // Before the slot can be handed out again, so that no later peer inherits what this one had. A peer that never
    // had a bit set has none to clear: a connection that carried no Sentinel message costs no pass over the table.
    if (m_slot_dirty[slot]) {
        for (auto& [key, entry] : m_entries) {
            if (word < entry.bits.size()) entry.bits[word] &= keep;
        }
    }
    m_slot_dirty[slot] = false;
    m_owner[slot] = NO_PEER;
    m_slot_of.erase(it);
}

void SeenTable::SetCapacity(size_t capacity)
{
    LOCK(m_mutex);
    m_capacity = capacity;
}

void SeenTable::DropOldestEpoch()
{
    const auto oldest = m_keys_of_epoch.begin();
    for (const uint256& key : oldest->second) m_entries.erase(key);
    m_keys_of_epoch.erase(oldest);
}

void SeenTable::Expire(uint32_t tip_epoch)
{
    LOCK(m_mutex);
    if (tip_epoch == 0) return;
    const uint32_t oldest_kept{tip_epoch - 1};
    while (!m_keys_of_epoch.empty() && m_keys_of_epoch.begin()->first < oldest_kept) DropOldestEpoch();
}

void SeenTable::ForgetFrom(uint32_t epoch)
{
    LOCK(m_mutex);
    for (auto it = m_keys_of_epoch.lower_bound(epoch); it != m_keys_of_epoch.end();) {
        for (const uint256& key : it->second) m_entries.erase(key);
        it = m_keys_of_epoch.erase(it);
    }
    // A note read before this point records what the store held before it forgot: it must not land.
    ++m_generation;
}

uint64_t SeenTable::Generation() const
{
    LOCK(m_mutex);
    return m_generation;
}

void SeenTable::OpenWindow()
{
    LOCK(m_mutex);
    ++m_open_windows;
}

void SeenTable::CloseWindow()
{
    LOCK(m_mutex);
    --m_open_windows;
}

bool SeenTable::Stale(uint64_t generation) const
{
    return generation != m_generation || m_open_windows > 0;
}

SeenTable::Entry* SeenTable::FindOrMake(const uint256& key, uint32_t epoch)
{
    if (const auto it = m_entries.find(key); it != m_entries.end()) return &it->second;
    // Room comes only from epochs older than the new entry's own, the oldest first: never newer entries for an older
    // message, nor its own epoch's for one more of it. Forgetting what a peer has only costs a send.
    while (m_entries.size() >= m_capacity && !m_keys_of_epoch.empty() && m_keys_of_epoch.begin()->first < epoch) {
        DropOldestEpoch();
    }
    if (m_entries.size() >= m_capacity) return nullptr;
    m_keys_of_epoch[epoch].push_back(key);
    return &m_entries.emplace(key, Entry{epoch, {}}).first->second;
}

void SeenTable::Set(Entry& entry, PeerId peer)
{
    const auto it = m_slot_of.find(peer);
    if (it == m_slot_of.end()) return; // not connected: there is nothing to withhold from it
    const size_t word = it->second / 64;
    if (entry.bits.size() <= word) entry.bits.resize(word + 1, 0);
    entry.bits[word] |= uint64_t{1} << (it->second % 64);
    m_slot_dirty[it->second] = true;
}

void SeenTable::NoteReceived(const uint256& key, uint32_t epoch, PeerId peer, uint64_t generation)
{
    LOCK(m_mutex);
    if (Stale(generation) || m_slot_of.count(peer) == 0) return;
    if (Entry* entry = FindOrMake(key, epoch)) Set(*entry, peer);
}

bool SeenTable::AnyConnected(const std::vector<PeerId>& peers) const
{
    for (const PeerId peer : peers) {
        if (m_slot_of.count(peer) != 0) return true;
    }
    return false;
}

void SeenTable::NoteSent(const uint256& key, uint32_t epoch, const std::vector<PeerId>& peers, uint64_t generation)
{
    LOCK(m_mutex);
    if (Stale(generation) || !AnyConnected(peers)) return;
    if (Entry* entry = FindOrMake(key, epoch)) {
        for (const PeerId peer : peers) Set(*entry, peer);
    }
}

std::vector<SeenTable::PeerId> SeenTable::BeginRelay(const uint256& key)
{
    LOCK(m_mutex);
    // Between the store's forgetting and the table's, the bits may stand for bytes the store has forgotten.
    if (m_open_windows > 0) return {};
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return {};
    if (it->second.relayed) {
        // Relayed before and taken as new again: the store forgot these bytes, and so may every peer. Start over.
        std::fill(it->second.bits.begin(), it->second.bits.end(), 0);
        return {};
    }
    return HoldersOf(it->second);
}

void SeenTable::NoteRelayed(const uint256& key, uint32_t epoch, const std::vector<PeerId>& peers)
{
    LOCK(m_mutex);
    // Made, if there is room, even for a relay that reached nobody: the mark is what makes the next take of these
    // bytes start over.
    if (Entry* entry = FindOrMake(key, epoch)) {
        entry->relayed = true;
        for (const PeerId peer : peers) Set(*entry, peer);
    }
}

std::vector<SeenTable::PeerId> SeenTable::HoldersOf(const Entry& entry) const
{
    std::vector<PeerId> holders;
    for (size_t word = 0; word < entry.bits.size(); ++word) {
        for (size_t bit = 0; bit < 64; ++bit) {
            if (((entry.bits[word] >> bit) & 1) == 0) continue;
            const size_t slot{word * 64 + bit};
            if (slot < m_owner.size() && m_owner[slot] != NO_PEER) holders.push_back(m_owner[slot]);
        }
    }
    std::sort(holders.begin(), holders.end());
    return holders;
}

std::vector<SeenTable::PeerId> SeenTable::Holders(const uint256& key) const
{
    LOCK(m_mutex);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return {};
    return HoldersOf(it->second);
}

size_t SeenTable::Size() const
{
    LOCK(m_mutex);
    return m_entries.size();
}

size_t SeenTable::Capacity() const
{
    LOCK(m_mutex);
    return m_capacity;
}

} // namespace dsl
