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
 * A win must never leave the winner with a coin that can no longer stake.
 *
 * The wallet halves an output every time it wins, and repeats that on every
 * later win, so an output walks down by a factor of two until it stops. Halving
 * without looking at where the halves land is what retires coins: below the
 * minimum stakeable amount, or exactly on a collateral amount, a coin is skipped
 * from then on with nothing said. Roughly a third of starting sizes reach such a
 * half.
 */
BOOST_AUTO_TEST_CASE(a_split_never_produces_an_output_that_cannot_stake)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);

    const auto stakeable = [&](CAmount value) {
        return value >= params.stakeValueRange[0] &&
               value != params.regularMnCollateral &&
               value != params.evoMnCollateral;
    };

    // Whatever it decides, the credit is preserved and a split leaves two
    // outputs that can both stake again.
    for (CAmount credit = 10000 * COIN; credit <= 60000 * COIN; credit += 137 * CENT) {
        const std::vector<CAmount> outputs = staker.SplitStakeCredit(credit, SPLIT_THRESHOLD);
        BOOST_REQUIRE(outputs.size() == 1 || outputs.size() == 2);

        CAmount sum = 0;
        for (const CAmount value : outputs) {
            sum += value;
        }
        BOOST_CHECK_EQUAL(sum, credit);

        if (outputs.size() == 2) {
            for (const CAmount value : outputs) {
                BOOST_CHECK_MESSAGE(stakeable(value),
                                    "splitting " << credit / COIN << " produced "
                                                 << value / COIN << ", which cannot stake");
            }
        }
    }

    // The band that used to be split into dust: anything from the threshold up
    // to twice the minimum has halves below the floor, so it must stay whole.
    for (CAmount credit = SPLIT_THRESHOLD; credit < 2 * params.stakeValueRange[0]; credit += 97 * CENT) {
        BOOST_CHECK_MESSAGE(staker.SplitStakeCredit(credit, SPLIT_THRESHOLD).size() == 1,
                            "a credit of " << credit / COIN << " was split into unusable halves");
    }

    // Exactly twice a collateral amount halves onto it from both sides.
    BOOST_CHECK_EQUAL(staker.SplitStakeCredit(2 * params.regularMnCollateral, SPLIT_THRESHOLD).size(), 1u);
    BOOST_CHECK_EQUAL(staker.SplitStakeCredit(2 * params.evoMnCollateral, SPLIT_THRESHOLD).size(), 1u);

    // A comfortable size still splits, or the wallet would stop spreading coins.
    BOOST_CHECK_EQUAL(staker.SplitStakeCredit(12000000 * COIN, SPLIT_THRESHOLD).size(), 2u);
}

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
    // GetDepthInMainChain() counts the coin's own block and the kernel does
    // not, so a coin the kernel accepts at COINBASE_MATURITY + 1 is one
    // deeper by the wallet's measure.
    const int deep = COINBASE_MATURITY + 2;
    const int before_v2 = params.nPosKernelV2ActivationHeight - 1;
    const int after_v2 = params.nPosKernelV2ActivationHeight;

    const auto why = [&](CAmount value, int depth, TxoutType type,
                         int64_t age, int height) {
        return staker.ClassifyForStaking(value, depth, type, age, height);
    };

    BOOST_CHECK(why(good, deep, TxoutType::PUBKEYHASH, min_age, before_v2) == StakeEligibility::Eligible);
    BOOST_CHECK(why(good, deep - 1, TxoutType::PUBKEYHASH, min_age, before_v2) == StakeEligibility::Immature);

    // The rule is not restricted to generated outputs: CheckProofOfStake applies
    // it to every staking input. A received coin this shallow answered Eligible
    // until the classifier was brought in line, and the selection loop then
    // spent kernel attempts on a coin the network would have refused.
    BOOST_CHECK(why(good, 1, TxoutType::PUBKEYHASH, min_age, before_v2) == StakeEligibility::Immature);

    BOOST_CHECK(why(good, deep, TxoutType::BLSPUBKEY, min_age, before_v2) == StakeEligibility::BLSAddress);

    BOOST_CHECK(why(params.stakeValueRange[0] - 1, deep, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::BelowMin);
    BOOST_CHECK(why(params.stakeValueRange[1] + 1, deep, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::AboveMax);

    BOOST_CHECK(why(params.regularMnCollateral, deep, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::Collateral);
    BOOST_CHECK(why(params.evoMnCollateral, deep, TxoutType::PUBKEYHASH, min_age, before_v2)
                == StakeEligibility::Collateral);

    BOOST_CHECK(why(good, deep, TxoutType::PUBKEYHASH, min_age - 1, before_v2) == StakeEligibility::TooYoung);

    // The age cap is one of the rules the activation height lifts, and coin
    // selection has to follow validation across that line or the wallet either
    // builds blocks the network rejects or skips coins it would accept.
    BOOST_CHECK(why(good, deep, TxoutType::PUBKEYHASH, max_age + 1, before_v2) == StakeEligibility::TooOld);
    BOOST_CHECK(why(good, deep, TxoutType::PUBKEYHASH, max_age + 1, after_v2) == StakeEligibility::Eligible);
}

BOOST_AUTO_TEST_SUITE_END()
