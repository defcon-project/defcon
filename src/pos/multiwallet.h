#ifndef BITCOIN_POS_MULTIWALLET_H
#define BITCOIN_POS_MULTIWALLET_H

#include <chainparams.h>
#include <consensus/params.h>
#include <pos/stake.h>

void MultiwalletInitialize();
void MultiwalletMaintenance();
bool ToggleWalletStaking(const std::string& name);
int ReturnActiveStakingWallets();
bool IsWalletStaking(const std::string& name);

#endif // BITCOIN_POS_MULTIWALLET_H
