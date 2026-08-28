// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <pos/stake.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(pos_stake_rules_tests, WalletTestingSetup)

namespace {
//! Mainnet's stake limits, stated here so the rules under test are visible
//! rather than inherited from whichever network the fixture selected.
Consensus::Params MainnetLikeLimits()
{
    Consensus::Params params = Params().GetConsensus();
    params.stakeValueRange = {10000 * COIN, 12500000 * COIN};
    params.regularMnCollateral = 1000000 * COIN;
    params.evoMnCollateral = 4000000 * COIN;
    params.nPosKernelV2ActivationHeight = 1000;
    return params;
}

constexpr CAmount SPLIT_THRESHOLD = 15000 * COIN;
} // namespace

/**
 * Every reason a coin is held back has a name.
 *
 * Seven rules can each remove a coin from the staking loop, and one of them
 * removes it permanently. They shared a single outcome -- a `continue` -- so a
 * wallet with a full balance and no weight had nothing to consult. One
 * classifier now answers for the loop and for getstakinginfo alike, which is
 * what stops the report and the rule from drifting apart.
 */
BOOST_AUTO_TEST_CASE(every_exclusion_has_a_name)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);

    const CAmount good = 100000 * COIN;
    const int64_t min_age = params.stakeAgeRange[0];
    const int64_t max_age = params.stakeAgeRange[1];
    const int deep = COINBASE_MATURITY + 1;
    const int before_v2 = params.nPosKernelV2ActivationHeight - 1;
    const int after_v2 = params.nPosKernelV2ActivationHeight;

    const auto why = [&](CAmount value, bool generated, int depth, TxoutType type,
                         int64_t age, int height) {
        return staker.ClassifyForStaking(value, generated, depth, type, age, height);
    };

    BOOST_CHECK(why(good, false, 0, TxoutType::PUBKEYHASH, min_age, before_v2) == StakeEligibility::Eligible);

    BOOST_CHECK(why(good, true, deep - 1, TxoutType::PUBKEYHASH, min_age, before_v2) == StakeEligibility::Immature);
    BOOST_CHECK(why(good, true, deep, TxoutType::PUBKEYHASH, min_age, before_v2) == StakeEligibility::Eligible);

    BOOST_CHECK(why(good, false, 0, TxoutType::BLSPUBKEY, min_age, before_v2) == StakeEligibility::BLSAddress);

    BOOST_CHECK(why(params.stakeValueRange[0] - 1, false, 0, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::BelowMin);
    BOOST_CHECK(why(params.stakeValueRange[1] + 1, false, 0, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::AboveMax);

    BOOST_CHECK(why(params.regularMnCollateral, false, 0, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::Collateral);
    BOOST_CHECK(why(params.evoMnCollateral, false, 0, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::Collateral);

    BOOST_CHECK(why(good, false, 0, TxoutType::PUBKEYHASH, min_age - 1, before_v2) == StakeEligibility::TooYoung);

    // The age cap is one of the rules the activation height lifts, and coin
    // selection has to follow validation across that line or the wallet either
    // builds blocks the network rejects or skips coins it would accept.
    BOOST_CHECK(why(good, false, 0, TxoutType::PUBKEYHASH, max_age + 1, before_v2) == StakeEligibility::TooOld);
    BOOST_CHECK(why(good, false, 0, TxoutType::PUBKEYHASH, max_age + 1, after_v2) == StakeEligibility::Eligible);
}

BOOST_AUTO_TEST_SUITE_END()
