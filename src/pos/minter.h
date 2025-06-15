// Copyright (c) 2025 The Pacplatform Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MINTER_H
#define MINTER_H

#include <node/context.h>

#include <thread>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <string>

class CWallet;
class CBlock;

extern std::atomic<bool> fTryToSync;
extern std::atomic<bool> fIsStaking;

bool CheckStake(ChainstateManager& chainman, CBlock *pblock);
void StopStaking();
void ThreadStakeMiner(NodeContext& node);

#endif // MINTER_H

