// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/doublesign.h>

#include <logging.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <script/standard.h>
#include <tinyformat.h>

#include <algorithm>
#include <exception>
#include <string>

namespace {
std::string KernelText(const COutPoint& kernel)
{
    return strprintf("%s:%u", kernel.hash.ToString(), kernel.n);
}

template <typename Blocks>
bool Holds(const Blocks& blocks, const uint256& hash)
{
    return std::any_of(blocks.begin(), blocks.end(), [&](const auto& seen) { return seen.hash == hash; });
}

/**
 * The key as one uncompressed encoding, or nullopt if the bytes are no
 * secp256k1 key. One key has three encodings -- compressed (02/03),
 * uncompressed (04) and hybrid (06/07) -- and a block signature verifies
 * under any of them, so the owner is compared by key, not by bytes.
 */
std::optional<CPubKey> KeyPoint(const std::vector<unsigned char>& bytes)
{
    CPubKey key{bytes};
    if (!key.Decompress()) return std::nullopt;
    return key;
}

//! Whether `id` hashes any encoding of `point` (an uncompressed key).
bool PaysKeyHash(const CPubKey& point, const CKeyID& id)
{
    if (point.GetID() == id) return true;
    const unsigned char odd = point[CPubKey::SIZE - 1] & 1;
    std::vector<unsigned char> bytes(point.begin(), point.end());
    bytes[0] = 0x06 | odd; // hybrid
    if (CPubKey{bytes}.GetID() == id) return true;
    bytes[0] = 0x02 | odd; // compressed: the x coordinate alone
    bytes.resize(CPubKey::COMPRESSED_SIZE);
    return CPubKey{bytes}.GetID() == id;
}

//! Reports a failure of the observer; nothing it does, failing included, leaves here.
void ReportFailure(const char* what) noexcept
{
    try {
        LogPrintf("ObserveStakeDoubleSign: %s\n", what);
    } catch (...) {
    }
}
} // namespace

bool StakeDoubleSignMonitor::SignerOwnsKernel(const CScript& signer, const CScript& kernel_script)
{
    std::vector<std::vector<unsigned char>> signer_solutions;
    if (Solver(signer, signer_solutions) != TxoutType::PUBKEY) return false;
    const std::optional<CPubKey> key{KeyPoint(signer_solutions[0])};
    if (!key) return false;
    std::vector<std::vector<unsigned char>> kernel_solutions;
    switch (Solver(kernel_script, kernel_solutions)) {
    case TxoutType::PUBKEY:
        return KeyPoint(kernel_solutions[0]) == key;
    case TxoutType::PUBKEYHASH:
        return PaysKeyHash(*key, CKeyID{uint160{kernel_solutions[0]}});
    default:
        return false;
    }
}

bool StakeDoubleSignMonitor::Observe(int tip_height, int height, const uint256& hash, const COutPoint& kernel,
                                     const CScript& signer, const std::optional<CScript>& kernel_script)
{
    LOCK(m_mutex);
    // The window follows the active chain, not the highest block offered.
    const int floor = tip_height - WINDOW;
    while (!m_slots.empty() && m_slots.begin()->first.first < floor) m_slots.erase(m_slots.begin());
    if (height < floor) return false;

    const Key key{height, kernel};
    auto it = m_slots.find(key);
    const CScript* owner = it != m_slots.end() ? &it->second.owner : (kernel_script ? &*kernel_script : nullptr);
    if (owner == nullptr) {
        LogPrint(BCLog::POS, "%s: block %s at height %d: kernel %s cannot be read; not tracked\n",
                 __func__, hash.ToString(), height, KernelText(kernel));
        return false;
    }
    // Only the owner's blocks are kept: what anyone can write takes no room.
    if (!SignerOwnsKernel(signer, *owner)) {
        LogPrint(BCLog::POS, "%s: block %s at height %d names kernel %s under another key; not counted\n",
                 __func__, hash.ToString(), height, KernelText(kernel));
        return false;
    }
    if (it == m_slots.end()) {
        if (m_slots.size() >= MAX_KERNELS) {
            LogPrint(BCLog::POS, "%s: block %s at height %d: no room for kernel %s; not tracked\n",
                     __func__, hash.ToString(), height, KernelText(kernel));
            return false;
        }
        it = m_slots.emplace(key, Slot{*owner, {}, false}).first;
    }
    Slot& slot = it->second;
    if (Holds(slot.blocks, hash)) return false; // this block again
    if (slot.blocks.size() < MAX_BLOCKS_PER_KERNEL) slot.blocks.push_back({hash, signer});
    if (slot.blocks.size() < 2) return false;
    if (slot.counted) {
        LogPrint(BCLog::POS, "%s: one more block %s at height %d from the owner of kernel %s\n",
                 __func__, hash.ToString(), height, KernelText(kernel));
        return false;
    }
    slot.counted = true;
    ++m_count;
    m_last = Event{height, kernel, slot.blocks[0].hash, slot.blocks[1].hash};
    LogPrintf("Double-signed stake: blocks %s and %s at height %d both spend kernel %s and are signed by its owner\n",
              slot.blocks[0].hash.ToString(), slot.blocks[1].hash.ToString(), height, KernelText(kernel));
    return true;
}

StakeDoubleSignMonitor::Summary StakeDoubleSignMonitor::GetSummary() const
{
    LOCK(m_mutex);
    return Summary{m_count, m_last};
}

size_t StakeDoubleSignMonitor::KernelsTracked() const
{
    LOCK(m_mutex);
    return m_slots.size();
}

StakeDoubleSignMonitor& GetStakeDoubleSignMonitor()
{
    static StakeDoubleSignMonitor monitor;
    return monitor;
}

void ObserveStakeDoubleSign(const CBlock& block, int height, const uint256& hash, int tip_height,
                            const std::function<std::optional<CScript>(const COutPoint&)>& read_kernel)
{
    if (!block.IsProofOfStake() || block.vtx[1]->vin.empty() || block.vtx[1]->vout.size() < 2) return;
    // Observation must never cost a block its acceptance: whatever fails here,
    // the read, the record or the log line that reports the failure (which needs
    // memory too), stays here.
    try {
        const COutPoint& kernel = block.vtx[1]->vin[0].prevout;
        GetStakeDoubleSignMonitor().Observe(tip_height, height, hash, kernel, block.vtx[1]->vout[1].scriptPubKey, read_kernel(kernel));
    } catch (const std::exception& e) {
        ReportFailure(e.what());
    } catch (...) {
        ReportFailure("unknown failure");
    }
}
