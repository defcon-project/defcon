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

/**
 * Whether the miner is inside its loop right now.
 *
 * This is deliberately not derived from wallet state. Every field
 * getstakinginfo reports describes a wallet -- a balance-derived weight, a
 * difficulty, an excluded-value breakdown -- and a wallet looks identical
 * whether or not anything is trying to stake it. When the minter thread died
 * on its first attempt, each of eight nodes went on reporting staking:true
 * with a full weight, and counting `threadstakeminer thread start` against
 * `thread exit` in the log was the only way to tell.
 *
 * ThreadStakeMiner now restarts PoSMiner rather than giving up, which makes
 * the answer more useful, not less: a recurring fault no longer stops the
 * node, so nothing else marks it, and a node can retry forever without
 * anything saying so.
 */
extern std::atomic<bool> fMinterRunning;

/**
 * The minter's last failure, and when it happened; empty when it has not
 * failed. Recorded separately from the log because a JSONRPCError is a
 * UniValue rather than a std::exception, so it carries no what() to print --
 * the handler could only log that *something* went wrong.
 */
std::string MinterLastError();
int64_t MinterLastErrorTime();
void SetMinterLastError(const std::string& what);

bool CheckStake(ChainstateManager& chainman, CBlock *pblock);
void StopStaking();
void ThreadStakeMiner(NodeContext& node);

#endif // MINTER_H

