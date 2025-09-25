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

void MultiwalletMaintenance()
{
    LOCK(stakable_mutex);

    std::vector<CStakeWallet> temp_wallets;
    temp_wallets = stakable_wallets;

    stakable_sz = 0;
    stakable_wallets.clear();
    const Consensus::Params& params = Params().GetConsensus();

    for (auto p : GetWallets())
    {
        CStakeWallet wallet(p.get(), (Consensus::Params&) params);
        wallet.StakingDisabled();
        //use temporary copy of old wallet list to determine which wallets were staking
        for (int i=0; i<temp_wallets.size(); i++) {
            if (wallet.GetWallet()->GetName() == temp_wallets[i].GetWallet()->GetName()) {
                if (temp_wallets[i].CanStake()) {
                    wallet.StakingEnabled();
                }
            }
        }
        stakable_wallets.push_back(wallet);
        stakable_sz++;
    }
}

void ToggleWalletStaking(const std::string& name)
{
    LOCK(stakable_mutex);

    for (int y = 0; y < stakable_sz; y++)
    {
        CWallet* this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;
        if (name == this_wallet->GetName()) {
            bool current = stakable_wallets[y].CanStake();
            if (!current) {
                stakable_wallets[y].StakingEnabled();
                LogPrint(BCLog::POS, "%s - enabling staking for wallet '%s'\n", __func__, name);
            } else {
                stakable_wallets[y].StakingDisabled();
                LogPrint(BCLog::POS, "%s - disabling staking for wallet '%s'\n", __func__, name);
            }
            break;
        }
    }
}

int ReturnActiveStakingWallets()
{
    LOCK(stakable_mutex);

    int active_wallets = 0;
    for (int y = 0; y < stakable_sz; y++)
    {
        CWallet* this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;
        if (stakable_wallets[y].CanStake())
            active_wallets += 1;
    }
    return active_wallets;
}

bool IsWalletStaking(const std::string& name)
{
    LOCK(stakable_mutex);

    for (int y = 0; y < stakable_sz; y++)
    {
        CWallet* this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;
        if (name == this_wallet->GetName()) {
            return stakable_wallets[y].CanStake();
        }
    }

    //uhm?
    return false;
}
