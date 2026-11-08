// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_SEEN_H
#define BITCOIN_EVO_POSE_SERVICE_SEEN_H

#include <span.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsl {

/** Which connected peers already have which exact Sentinel message (sentinelfix.md 4.3).
 *
 *  The key is Key(type, payload), the SHA-256 of the message type and its exact serialized payload; the value is the
 *  message's epoch, a bitmap of peer slots and a mark that this node has relayed the message. A peer gets a slot when
 *  it connects (AddPeer) and gives it back when it disconnects (RemovePeer), which first clears the slot's bit from
 *  every entry, so a later peer never inherits it. A bit is set when the peer sent us exactly these bytes, a copy
 *  dropped undecoded included (NoteReceived), or when we sent them to it (NoteSent, NoteRelayed), and the first relay
 *  of the bytes leaves out the peers BeginRelay() names. That is the table's only effect: it withholds a send only of
 *  bytes the peer demonstrably has on its current connection. A full table, an expired or missing entry, or a peer
 *  without a slot can only cause a send that was not needed, never withhold a first delivery; there is no
 *  probabilistic filter.
 *
 *  It follows the store rather than keeping a memory of its own. Every relay is of a message the store (or the
 *  manager) has just taken as new, so a second relay of the same bytes means the store forgot them and took them
 *  again -- after a rewind back across the epoch's end that keeps its base, for one, or a reorg away from and back
 *  onto the same base, when the peers may have forgotten them as well. NoteRelayed() therefore marks the entry, and
 *  BeginRelay() starts a marked entry over: every later relay withholds from nobody. So BeginRelay() reads an entry's
 *  bits only at its first relay; the bits a marked entry gathers afterwards are kept, as 4.3/2 asks, but never read.
 *  On such a reorg the caller also drops, with ForgetFrom(), the epochs the store dropped, marked or not.
 *  NoteRelayed() makes the entry if there is room, even for a relay that reached nobody; in a full table with no older
 *  epoch to drop it makes none, and an entry that Expire() or the capacity drops takes its mark with it. A later copy
 *  can then make the entry again, unmarked, and a take after the store forgot the bytes counts as a first one --
 *  unless the forgetting was a reorg the caller followed with ForgetFrom().
 *
 *  Following a reorg is ordered against the threads that note and relay. The store forgets first (BeginEpoch), the
 *  table after it (ForgetFrom), and a note records something its thread saw earlier -- a copy the store held, a send.
 *  So a caller reads Generation() before it looks at the store or sends, and passes it to the note: ForgetFrom()
 *  advances the generation, and a note read before a forget is dropped, so it cannot bring back, unmarked, a bit the
 *  forget took. And the caller brackets the store's forgetting and its own ForgetFrom() in a ForgetWindow: while one is
 *  open, BeginRelay() names nobody and every note is dropped, so nothing reads or writes the bits between the two
 *  forgettings. Both can only cost a send.
 *
 *  Bounds: Expire() drops the entries of the epochs before the one before the tip's. At the capacity a new entry
 *  makes room only by dropping whole epochs older than its own, the oldest first, and is not made if that does not
 *  make room. Unlike the letter of sentinelfix.md 4.3/5 (the oldest epoch goes), neither its own epoch nor a newer one
 *  ever goes for it: a refused entry withholds nothing, and the newer knowledge stays. A lower capacity drops nothing
 *  by itself. The entries are indexed by epoch, so each entry made or dropped costs a logarithmic step (amortized).
 *  RemovePeer() walks the entries for a slot that had a bit set; in steady state that is nearly every peer carrying
 *  Sentinel traffic, so such a disconnect walks up to the capacity under the table's lock (4.3/1 requires the clear).
 *  Thread-safe: the message thread notes what it reads, the message thread and the validation queue's thread relay
 *  and forget, the network thread removes peers. */
class SeenTable
{
public:
    using PeerId = int64_t;

    //! The key of a message: equal keys mean the same type and the same bytes.
    static uint256 Key(const std::string& msg_type, Span<const unsigned char> payload);

    void AddPeer(PeerId peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void RemovePeer(PeerId peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The most entries a new entry may join; lowering it drops nothing by itself.
    void SetCapacity(size_t capacity) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Drops the entries of every epoch before tip_epoch - 1.
    void Expire(uint32_t tip_epoch) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Drops the entries of this epoch and of every later one: the store dropped them in a reorg. Advances the
    //! generation.
    void ForgetFrom(uint32_t epoch) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! How many times ForgetFrom() has run. Read before looking at the store or sending, and passed to the note.
    uint64_t Generation() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The peer sent us these bytes; generation is Generation() as read before the caller looked at the store. Dropped
    //! if a ForgetFrom() has run since, or while a ForgetWindow is open.
    void NoteReceived(const uint256& key, uint32_t epoch, PeerId peer, uint64_t generation) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! We sent these bytes to these peers, outside a relay (an answer to a request); generation as for NoteReceived(),
    //! read before the send.
    void NoteSent(const uint256& key, uint32_t epoch, const std::vector<PeerId>& peers, uint64_t generation)
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Before a relay of these bytes: the connected peers that have them, in ascending order -- none if the entry is
    //! marked as relayed, which then starts over (the store forgot the bytes and took them again), and none while a
    //! ForgetWindow is open, which leaves the entry as it is.
    std::vector<PeerId> BeginRelay(const uint256& key) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! After a relay: these peers were sent the bytes. Marks the entry as relayed, making it if there is room, even when
    //! no peer is named.
    void NoteRelayed(const uint256& key, uint32_t epoch, const std::vector<PeerId>& peers) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The connected peers whose bits are set for these bytes, in ascending order.
    std::vector<PeerId> Holders(const uint256& key) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Capacity() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Open from its construction to its destruction: around a step in which the store may forget, up to and with the
    //! ForgetFrom() that follows it.
    class ForgetWindow
    {
    public:
        explicit ForgetWindow(SeenTable& table) : m_table{table} { m_table.OpenWindow(); }
        ~ForgetWindow() { m_table.CloseWindow(); }
        ForgetWindow(const ForgetWindow&) = delete;
        ForgetWindow& operator=(const ForgetWindow&) = delete;

    private:
        SeenTable& m_table;
    };

private:
    void OpenWindow() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void CloseWindow() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Whether a note read at this generation may no longer stand: a forget since, or a window open now.
    bool Stale(uint64_t generation) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    struct Entry {
        uint32_t epoch;
        std::vector<uint64_t> bits;
        //! Relayed by this node at least once.
        bool relayed{false};
    };

    //! The connected peers whose bits are set in the entry, in ascending order.
    std::vector<PeerId> HoldersOf(const Entry& entry) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! Whether any of these peers holds a slot.
    bool AnyConnected(const std::vector<PeerId>& peers) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    static constexpr PeerId NO_PEER{-1};

    //! The entry of a key, made if there is none and there is room for it; nullptr otherwise.
    Entry* FindOrMake(const uint256& key, uint32_t epoch) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! Drops every entry of the oldest epoch in the table, which must not be empty.
    void DropOldestEpoch() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! Sets the peer's bit, if the peer holds a slot.
    void Set(Entry& entry, PeerId peer) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    mutable Mutex m_mutex;
    std::map<uint256, Entry> m_entries GUARDED_BY(m_mutex);
    //! The keys of every entry, by the entry's epoch: each key of m_entries once, under its entry's epoch.
    std::map<uint32_t, std::vector<uint256>> m_keys_of_epoch GUARDED_BY(m_mutex);
    std::map<PeerId, size_t> m_slot_of GUARDED_BY(m_mutex);
    //! The peer holding each slot, NO_PEER for a free one.
    std::vector<PeerId> m_owner GUARDED_BY(m_mutex);
    //! Per slot: a bit was set for its peer since the peer took the slot, so its removal may have bits to clear.
    std::vector<bool> m_slot_dirty GUARDED_BY(m_mutex);
    size_t m_capacity GUARDED_BY(m_mutex){0};
    uint64_t m_generation GUARDED_BY(m_mutex){0};
    int m_open_windows GUARDED_BY(m_mutex){0};
};

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_SEEN_H
