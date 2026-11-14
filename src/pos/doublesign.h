// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_DOUBLESIGN_H
#define BITCOIN_POS_DOUBLESIGN_H

#include <consensus/amount.h>
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
class CBlockIndex;
namespace Consensus {
struct Params;
} // namespace Consensus

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
 * The owner, though, can sign a block on any output of their own, and blocks
 * are observed when they are accepted, before their kernel is checked
 * (CheckProofOfStake runs when a block is connected). A block counts only if
 * its kernel meets the stake rules for it on its own branch: the depth, value
 * and age of the kernel's output and the kernel hash under the block's nBits
 * and nTime, the checks CheckProofOfStake makes but the coinstake's signature.
 * That is decided only where it can be: the block's parent is on the active
 * chain (a parent not yet connected still carries the stake modifier its
 * header gave it), and the block that created the kernel's output is an
 * ancestor of that parent (its height and time are this branch's). A block
 * that fails, or cannot be judged, is kept nowhere, so a count needs two
 * blocks that could each have been staked.
 *
 * What it does not see: a second block this node does not accept (an
 * unrequested block needs at least the tip's work, so in practice only a
 * sibling of the tip that arrives before the next block does); a kernel whose
 * output it cannot read when the first block on it arrives (already spent,
 * for example after a restart), which is not tracked at all; a kernel that is
 * neither pay-to-pubkey nor pay-to-pubkey-hash, which has no owner key; a
 * block whose parent is not on the active chain, or whose kernel was created
 * on another branch than the one recorded, which is not judged; heights more
 * than WINDOW below the tip; and new pairs once MAX_KERNELS are held.
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

    /** What is known of a kernel's output, read from the UTXO set with the first block on it. */
    struct KernelCoin {
        CScript script;       //!< whom it pays
        CAmount value{0};
        int height{0};        //!< of the block that created it, on the active chain when read
        uint256 funding;      //!< that block's hash
        // NOLINTNEXTLINE(google-explicit-constructor): a script alone is a coin of unknown value and origin
        KernelCoin(CScript script_in = {}, CAmount value_in = 0, int height_in = 0, const uint256& funding_in = {})
            : script{std::move(script_in)}, value{value_in}, height{height_in}, funding{funding_in} {}
    };

    /** Whether the block being observed could stake on its kernel (see the class comment). */
    enum class StakeVerdict { QUALIFIES, FAILS, UNDECIDED };
    using StakeCheck = std::function<StakeVerdict(const COutPoint& kernel, const KernelCoin& coin)>;

    /** The kernel rule applied (StakeKernelQualifies in pos/kernel.h; a stand-in in tests). */
    using KernelRule = bool (*)(const CBlockIndex* pindexPrev, const Consensus::Params& params, const COutPoint& prevout,
                                CAmount value, int coin_height, int64_t nBlockFromTime, int64_t nTime, unsigned int nBits);

    /** What the stake rules need of the block being observed. Plain data: building it cannot fail. */
    struct StakeContext {
        //! The block's parent if it is on the active chain, else nullptr: then the block is not judged.
        const CBlockIndex* settled_parent{nullptr};
        uint32_t time{0};
        uint32_t bits{0};
        const Consensus::Params* params{nullptr};
        KernelRule rule{nullptr};
    };

    /**
     * The stake rules on the block's own branch: UNDECIDED without a settled parent, or when the
     * block that created the output (`coin.funding` at `coin.height`) is not an ancestor of it;
     * otherwise the context's kernel rule, with that block's time.
     */
    static StakeVerdict Judge(const StakeContext& context, const COutPoint& kernel, const KernelCoin& coin);

    /**
     * Record an accepted block whose block signature has been checked.
     * `tip_height` is the active chain's height, which sets the window.
     * `coin` is the kernel's output if the caller could read it; without it, a
     * pair not already held is not tracked, and a pair held is judged by the
     * output recorded with its first block. `could_stake` judges this block,
     * by `coin` when there is one.
     * Returns true when this completes a double signature not counted before;
     * each height and kernel pair is counted once.
     */
    bool Observe(int tip_height, int height, const uint256& hash, const COutPoint& kernel, const CScript& signer,
                 const std::optional<KernelCoin>& coin, const StakeCheck& could_stake) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
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
        KernelCoin coin;          //!< the kernel's output; its script names the owner
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
 * CheckBlock verified its block signature). `read_kernel` returns the kernel's
 * output if it can; `stake` is what judging the block needs (Judge). Proof-of-
 * work blocks are ignored, and a failure here, the read and the judgement
 * included, is logged and never reaches the caller; neither does a failure to
 * log it.
 */
void ObserveStakeDoubleSign(const CBlock& block, int height, const uint256& hash, int tip_height,
                            const std::function<std::optional<StakeDoubleSignMonitor::KernelCoin>(const COutPoint&)>& read_kernel,
                            const StakeDoubleSignMonitor::StakeContext& stake);

#endif // BITCOIN_POS_DOUBLESIGN_H
