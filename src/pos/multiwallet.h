#ifndef BITCOIN_POS_MULTIWALLET_H
#define BITCOIN_POS_MULTIWALLET_H

#include <chainparams.h>
#include <consensus/params.h>
#include <pos/stake.h>

void MultiwalletInitialize();
void MultiwalletMaintenance();
bool ToggleWalletStaking(const std::string& name);
/** Put a wallet's staking switch into the given position, under the registry's
 *  lock, so a caller acting on what it last saw cannot flip the switch the
 *  wrong way. Returns the switch's position afterwards: false when the wallet
 *  is unknown, or when enabling was refused. */
bool SetWalletStaking(const std::string& name, bool enable);
int ReturnActiveStakingWallets();
bool IsWalletStaking(const std::string& name);

#endif // BITCOIN_POS_MULTIWALLET_H
