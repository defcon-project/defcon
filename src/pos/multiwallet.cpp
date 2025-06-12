// Copyright (c) 2025 The Pacplatform Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/multiwallet.h>

int stakable_sz;
RecursiveMutex stakable_mutex;
std::vector<CStakeWallet> stakable_wallets;

void MultiwalletInitialize()
{
    LOCK(stakable_mutex);

    stakable_sz = 0;
    stakable_wallets.clear();
    const Consensus::Params& params = Params().GetConsensus();

    for (auto p : GetWallets())
    {
        CStakeWallet wallet(p.get(), (Consensus::Params&) params);
        wallet.StakingDisabled();
        stakable_wallets.push_back(wallet);
        stakable_sz++;
    }

    LogPrint(BCLog::POS, "%s - found %d stakable wallets\n", __func__, stakable_sz);
}
