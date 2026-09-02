// Copyright (c) 2025 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/multiwallet.h>

#include <set>

int stakable_sz;
RecursiveMutex stakable_mutex;
std::vector<CStakeWallet> stakable_wallets;

void MultiwalletInitialize()
{
    LOCK(stakable_mutex);

    // The miner re-enters here when ThreadStakeMiner restarts it after an
    // unexpected failure. Rebuilding the list must not cost any wallet its
    // staking switch: nothing downstream ever turns it back on, and RPC
    // cannot show that it went off -- getstakinginfo reads wallet state, not
    // this list.
    std::set<std::string> was_staking;
    for (auto& entry : stakable_wallets) {
        if (entry.CanStake())
            was_staking.insert(entry.GetName());
    }

    stakable_sz = 0;
    stakable_wallets.clear();
    const Consensus::Params& params = Params().GetConsensus();

    for (auto p : GetWallets())
    {
        CStakeWallet wallet(p, (Consensus::Params&) params);
        if (was_staking.count(wallet.GetName())) {
            wallet.StakingEnabled();
        } else {
            wallet.StakingDisabled();
        }
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
        CStakeWallet wallet(p, (Consensus::Params&) params);
        wallet.StakingDisabled();
        //use temporary copy of old wallet list to determine which wallets were staking
        for (size_t i = 0; i < temp_wallets.size(); i++) {
            if (wallet.GetName() == temp_wallets[i].GetName()) {
                if (temp_wallets[i].CanStake()) {
                    wallet.StakingEnabled();
                }
            }
        }
        stakable_wallets.push_back(wallet);
        stakable_sz++;
    }
}

bool ToggleWalletStaking(const std::string& name)
{
    LOCK(stakable_mutex);

    for (int y = 0; y < stakable_sz; y++)
    {
        if (name == stakable_wallets[y].GetName()) {
            if (!stakable_wallets[y].CanStake()) {
                // Every enable ends up here, so this is where the wallet gets
                // its coinstake descriptors: a descriptor wallet otherwise
                // pays its own coinstake to a script it does not track, and
                // watches its balance leave as it produces blocks. Enabling
                // without them would reintroduce exactly that, so a failure
                // keeps the switch off.
                const std::shared_ptr<CWallet> this_wallet = stakable_wallets[y].GetWallet();
                if (this_wallet == nullptr || !EnsureCoinstakeDescriptors(*this_wallet)) {
                    LogPrint(BCLog::POS, "%s - not enabling staking for wallet '%s': coinstake descriptors unavailable\n", __func__, name);
                    return false;
                }
                stakable_wallets[y].StakingEnabled();
                LogPrint(BCLog::POS, "%s - enabling staking for wallet '%s'\n", __func__, name);
                return true;
            }
            stakable_wallets[y].StakingDisabled();
            LogPrint(BCLog::POS, "%s - disabling staking for wallet '%s'\n", __func__, name);
            return false;
        }
    }
    return false;
}

int ReturnActiveStakingWallets()
{
    LOCK(stakable_mutex);

    int active_wallets = 0;
    for (int y = 0; y < stakable_sz; y++)
    {
        const std::shared_ptr<CWallet> this_wallet = stakable_wallets[y].GetWallet();
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
        if (name == stakable_wallets[y].GetName()) {
            return stakable_wallets[y].CanStake();
        }
    }

    //uhm?
    return false;
}
