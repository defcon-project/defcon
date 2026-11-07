// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_STAKINGPACE_H
#define BITCOIN_POS_STAKINGPACE_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

class ArgsManager;
class CChainParams;

/**
 * The pace of the staking thread, and the one place a test may change it.
 *
 * The staking thread pauses between rounds and after every block it finds
 * (DEFAULT_STAKING_POLL_MS), and four times as long while it waits for the
 * chain, the masternode sync or the clock. These are wall-clock pauses, so on
 * regtest, where the kernel wins almost every attempt, they decide how long a
 * functional test waits for each staked block. -stakingpollms shortens them for
 * tests. It changes when the thread looks again, not the rules it applies when
 * it looks. Which search times it reaches does change: the search time is the
 * clock with posTimestampMask cleared, which does not always grow (with mask 5
 * it reads 8k, 8k, 8k+2, 8k+2, 8k, ... in every 8 seconds), and a search time
 * at or below the wallet's last searched one is skipped. At the production pace
 * the thread therefore sometimes reaches 8k+2 first and never tries 8k; a
 * shorter pace reaches both, so it makes somewhat more kernel attempts.
 *
 * Only devnet and regtest accept the argument. Everywhere else startup is
 * refused when it is set, and StakingPollInterval() returns the production
 * pause regardless, so the pace of a public network cannot be changed from the
 * configuration.
 */
namespace pos {

extern const char* const STAKING_POLL_ARG;

/** The production pause, in milliseconds: CStakeWallet::SHORTDELAY. */
static constexpr int64_t DEFAULT_STAKING_POLL_MS{2500};
/** The shortest pause allowed: below it the thread would spin rather than poll. */
static constexpr int64_t MIN_STAKING_POLL_MS{50};

/** True on the two chains whose staking pace a test may change: devnet and regtest. */
bool StakingPaceAllowedOn(const CChainParams& params);

/**
 * The startup gate as a pure function: the error to refuse startup with when
 * -stakingpollms is set on a chain that does not allow it, or to a value out of
 * range; otherwise nullopt. Not setting the argument is never an error.
 */
std::optional<std::string> StakingPaceRefusal(const CChainParams& params, const ArgsManager& args);

/**
 * The pause between staking rounds and after a found block: -stakingpollms on
 * devnet and regtest, DEFAULT_STAKING_POLL_MS everywhere else whatever the
 * configuration says.
 */
std::chrono::milliseconds StakingPollInterval(const CChainParams& params, const ArgsManager& args);

} // namespace pos

#endif // BITCOIN_POS_STAKINGPACE_H
