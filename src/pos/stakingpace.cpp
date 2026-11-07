// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/stakingpace.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <tinyformat.h>
#include <util/system.h>

#include <algorithm>

namespace pos {

const char* const STAKING_POLL_ARG = "-stakingpollms";

bool StakingPaceAllowedOn(const CChainParams& params)
{
    const std::string& network = params.NetworkIDString();
    return network == CBaseChainParams::REGTEST || network == CBaseChainParams::DEVNET;
}

std::optional<std::string> StakingPaceRefusal(const CChainParams& params, const ArgsManager& args)
{
    if (!args.IsArgSet(STAKING_POLL_ARG)) return std::nullopt;
    if (!StakingPaceAllowedOn(params)) {
        return strprintf("%s is not available on the %s chain: the staking thread keeps its production pace there",
                         STAKING_POLL_ARG, params.NetworkIDString());
    }
    // A value with no leading number ("fast", empty) or one that overflows reads
    // as 0 and is refused here with the rest; a leading number counts as that
    // number ("100ms" is 100).
    const int64_t ms = args.GetArg(STAKING_POLL_ARG, DEFAULT_STAKING_POLL_MS);
    if (ms < MIN_STAKING_POLL_MS || ms > DEFAULT_STAKING_POLL_MS) {
        return strprintf("%s must be between %d and %d milliseconds", STAKING_POLL_ARG,
                         MIN_STAKING_POLL_MS, DEFAULT_STAKING_POLL_MS);
    }
    return std::nullopt;
}

std::chrono::milliseconds StakingPollInterval(const CChainParams& params, const ArgsManager& args)
{
    if (!StakingPaceAllowedOn(params)) return std::chrono::milliseconds{DEFAULT_STAKING_POLL_MS};
    return std::chrono::milliseconds{std::clamp<int64_t>(args.GetArg(STAKING_POLL_ARG, DEFAULT_STAKING_POLL_MS),
                                                         MIN_STAKING_POLL_MS, DEFAULT_STAKING_POLL_MS)};
}

} // namespace pos
