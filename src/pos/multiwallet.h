#ifndef MULTIWALLET_H
#define MULTIWALLET_H

#include <chainparams.h>
#include <consensus/params.h>
#include <pos/stake.h>

void MultiwalletInitialize();
void MultiwalletMaintenance();
void ToggleWalletStaking(const std::string& name);
int ReturnActiveStakingWallets();
bool IsWalletStaking(const std::string& name);

#endif // MULTIWALLET_H
