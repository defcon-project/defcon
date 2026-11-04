// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_DOUBLESIGN_H
#define BITCOIN_POS_DOUBLESIGN_H

#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

class CBlock;

/**
 * Notices the owner of a kernel signing two different blocks with it at the
 * same height, and only notices it: nothing is refused, penalised or relayed
 * differently, and the record is not kept across a restart.
 *
 * A proof-of-stake block is signed with the key its coinstake's second output
 * pays (CheckBlockSignature, checked before a block is accepted). The kernel's
 * owner is whoever its own output pays, read from the UTXO set: an outpoint's
 * script never changes, so once read it holds for every block that names that
 * kernel. A block whose signer is the kernel's owner key -- in any of its
 * encodings, compressed, uncompressed or hybrid, since a signature verifies
 * under each -- was signed by the owner, whatever else is wrong with it;
 * anyone else can only put their own key there. So a double signature is two different blocks at one height, on one
 * kernel, each signed by that kernel's owner. Blocks signed by any other key
 * are not kept, so no number of them can hide or fake one.
 *
 * What it does not see: a second block this node does not accept (an
 * unrequested block needs at least the tip's work, so in practice only a
 * sibling of the tip that arrives before the next block does); a kernel whose
 * output it cannot read when the first block on it arrives (already spent,
 * for example after a restart), which is not tracked at all; a kernel that is
 * neither pay-to-pubkey nor pay-to-pubkey-hash, which has no owner key; heights
 * more than WINDOW below the tip; and new pairs once MAX_KERNELS are held.
 */
class StakeDoubleSignMonitor
{
public:
    /** Heights kept below the active chain's tip. */
    static constexpr int WINDOW{100};
    /** Height and kernel pairs kept at most, whatever they hold. */
    static constexpr size_t MAX_KERNELS{10000};
    /** Owner-signed blocks kept per height and kernel. */
    static constexpr size_t MAX_BLOCKS_PER_KERNEL{4};

    struct Event {
        int height{0};
        COutPoint kernel;
        uint256 first;
        uint256 second;
    };

    struct Summary {
        uint64_t count{0};
        std::optional<Event> last;
    };

    /**
     * Record an accepted block whose block signature has been checked.
     * `tip_height` is the active chain's height, which sets the window.
     * `kernel_script` is the kernel output's script if the caller could read it;
     * without it, a pair not already held is not tracked.
     * Returns true when this completes a double signature not counted before;
     * each height and kernel pair is counted once.
     */
    bool Observe(int tip_height, int height, const uint256& hash, const COutPoint& kernel, const CScript& signer,
                 const std::optional<CScript>& kernel_script) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Summary GetSummary() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Height and kernel pairs held now (for tests of the bound). */
    size_t KernelsTracked() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Whether `signer` (a pay-to-pubkey script) pays the key that `kernel_script`
     * pays, comparing keys, not their encodings: the kernel may name the key, or
     * its hash, in another encoding than the signer.
     */
    static bool SignerOwnsKernel(const CScript& signer, const CScript& kernel_script);

private:
    using Key = std::pair<int, COutPoint>;
    struct Seen {
        uint256 hash;
        CScript signer;
    };
    struct Slot {
        CScript owner;            //!< the kernel output's script
        std::vector<Seen> blocks; //!< blocks its owner signed
        bool counted{false};
    };

    mutable Mutex m_mutex;
    std::map<Key, Slot> m_slots GUARDED_BY(m_mutex);
    uint64_t m_count GUARDED_BY(m_mutex){0};
    std::optional<Event> m_last GUARDED_BY(m_mutex);
};

/** The node's monitor, fed by block acceptance and read by getblockchaininfo. */
StakeDoubleSignMonitor& GetStakeDoubleSignMonitor();

/**
 * Feed an accepted block to the node's monitor (block acceptance, after
 * CheckBlock verified its block signature). `read_kernel` returns the kernel
 * output's script if it can. Proof-of-work blocks are ignored, and a failure
 * here, the read included, is logged and never reaches the caller; neither
 * does a failure to log it.
 */
void ObserveStakeDoubleSign(const CBlock& block, int height, const uint256& hash, int tip_height,
                            const std::function<std::optional<CScript>(const COutPoint&)>& read_kernel);

#endif // BITCOIN_POS_DOUBLESIGN_H
