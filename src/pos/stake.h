// Copyright (c) 2025 The Pacplatform Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef STAKE_H
#define STAKE_H

#include <consensus/tx_verify.h>
#include <index/disktxpos.h>
#include <index/txindex.h>
#include <node/miner.h>
#include <pos/kernel.h>
#include <index/txindex.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>

using valtype = std::vector<unsigned char>;

/**
 * Convenience class allowing stake functions to have easy access to the wallet,
 * without the linking issues that come with later bitcoin releases.
 */
/**
 * Register the pay-to-pubkey form of a descriptor wallet's keys, so the wallet
 * recognises the coinstake outputs it produces. A no-op for legacy wallets.
 */
bool EnsureCoinstakeDescriptors(CWallet& wallet);

/** Why a coin is, or is not, available to the staking loop. */
enum class StakeEligibility {
    Eligible,
    Immature,    //!< not yet COINBASE_MATURITY + 1 deep in the kernel's terms
    BLSAddress,  //!< held at a BLS address, which cannot stake
    BelowMin,    //!< under stakeValueRange[0]
    AboveMax,    //!< over stakeValueRange[1]
    Collateral,  //!< exactly a masternode collateral amount
    TooYoung,    //!< under stakeAgeRange[0]
    TooOld,      //!< over stakeAgeRange[1], where that bound still applies
};

/**
 * What a wallet's coins were held back for, by value.
 *
 * A wallet can show a full balance and stake nothing at all, and until now the
 * only trace of why was a coin's absence from a loop: no message, no log line,
 * no count. Seven rules can each remove a coin silently and one of them removes
 * it for good, so the reason is worth reporting.
 */
struct StakeSkipReport
{
    CAmount immature{0};
    CAmount bls{0};
    CAmount below_min{0};
    CAmount above_max{0};
    CAmount collateral{0};
    CAmount too_young{0};
    CAmount too_old{0};

    void Add(StakeEligibility why, CAmount value);
    CAmount Total() const;
};

/**
 * The part of a report that will not resolve on its own.
 *
 * `immature` clears with depth and `too_young` with time, so both describe
 * coins that are on their way in. The rest do not move: an amount under the
 * floor, over the ceiling, equal to a masternode collateral, or held at a BLS
 * address stays out until someone spends it into a different shape, and
 * `too_old` only ever drifts further out of range.
 */
CAmount PermanentlyExcluded(const StakeSkipReport& report);

/**
 * Whether a wallet holds enough permanently-excluded value to be worth saying
 * so unprompted.
 *
 * The line is one whole stake's worth, taken from the network's own
 * stakeValueRange floor rather than invented: under it there is nothing a
 * consolidating transaction could turn back into a working stake, and at or
 * over it there is at least one.
 *
 * Deliberately not a percentage. The case this exists for was 700,800 DFCN
 * held permanently outside the rules by a wallet carrying 567 million -- worth
 * seventy stakes, and 0.12% of the balance. Any share-based threshold loose
 * enough to catch it would fire on almost anything.
 *
 * Pure, so the decision can be exercised without a wallet, a chain or a clock.
 */
bool ShouldWarnAboutExcludedValue(const StakeSkipReport& report, CAmount min_stake_value);

/** Names the non-zero permanent reasons, for the warning's text. */
std::string DescribePermanentExclusions(const StakeSkipReport& report);

class CStakeWallet
{
    private:
        bool staking;
        CWallet* wallet;
        std::string name;
        Consensus::Params params;

    public:
        static const int SHORTDELAY = 2500;
        static const int LARGEDELAY = 10000;

        CStakeWallet(CWallet* walletIn, Consensus::Params& paramsIn) {
            staking = false;
            wallet = walletIn;
            name = walletIn ? walletIn->GetName() : std::string{};
            params = paramsIn;
        }

        bool CanStake() { return staking; }
        void StakingEnabled() { staking = true; }
        void StakingDisabled() { staking = false; }

        void RemoveWallet() {
            wallet = nullptr;
        }

        CWallet* GetWallet() {
            return wallet;
        }

        // Captured at construction: an entry can outlive the wallet it points
        // to, so identity checks must not go through the pointer.
        const std::string& GetName() const { return name; }

        /**
         * The single answer to "may this coin stake", used by the selection loop
         * and by the report below. One implementation, so the two cannot drift
         * apart and start describing different rules.
         */
        StakeEligibility ClassifyForStaking(CAmount value, int depth,
                                            TxoutType type, int64_t inputAge, int nHeight) const;

        /** Tally every coin the wallet holds that the rules keep out. */
        StakeSkipReport ExplainExcludedCoins(int nHeight) const;

        /**
         * How a coinstake's credit is laid out: one output, or two that can each
         * stake again. Takes the threshold rather than reading it from the
         * wallet so the decision can be exercised on its own.
         */
        std::vector<CAmount> SplitStakeCredit(CAmount nCredit, CAmount threshold) const;

        uint64_t GetStakeWeight(int64_t nTime, int nHeight) const;
        bool SelectCoinsForStaking(CAmount nTargetValue, int64_t nTime, int nHeight, std::set<std::pair<const CWalletTx*, unsigned int>>& setCoinsRet, CAmount& nValueRet) const;
        bool CreateCoinStake(CChainState& chain_state, CBlockIndex* pindexPrev, unsigned int nBits, int64_t nTime, int nBlockHeight, int64_t nFees, CMutableTransaction& txNew, CKey& key);
        bool SignBlock(CChainState& chain_state, CBlockTemplate* pblocktemplate, int nHeight, int64_t nSearchTime);
};

#endif // STAKE_H
