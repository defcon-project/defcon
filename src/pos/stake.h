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

static const CAmount CENT = 1000000;

/**
 * Convenience class allowing stake functions to have easy access to the wallet,
 * without the linking issues that come with later bitcoin releases.
 */
class CStakeWallet
{
    private:
        bool staking;
        CWallet* wallet;
        Consensus::Params params;

    public:
        static const int SHORTDELAY = 2500;
        static const int LARGEDELAY = 10000;

        CStakeWallet(CWallet* walletIn, Consensus::Params& paramsIn) {
            staking = false;
            wallet = walletIn;
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

        uint64_t GetStakeWeight(int64_t nTime, int nHeight) const;
        bool SelectCoinsForStaking(CAmount nTargetValue, int64_t nTime, int nHeight, std::set<std::pair<const CWalletTx*, unsigned int>>& setCoinsRet, CAmount& nValueRet) const;
        bool CreateCoinStake(CChainState& chain_state, CBlockIndex* pindexPrev, unsigned int nBits, int64_t nTime, int nBlockHeight, int64_t nFees, CMutableTransaction& txNew, CKey& key);
        bool SignBlock(CChainState& chain_state, CBlockTemplate* pblocktemplate, int nHeight, int64_t nSearchTime);
};

#endif // STAKE_H
