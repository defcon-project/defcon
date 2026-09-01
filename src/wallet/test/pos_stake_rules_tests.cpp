// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <pos/stake.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <string>
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

    // The full stakeable rule -- both bounds and the collateral exclusions. If
    // it repeated the code's earlier partial predicate (no upper bound) it could
    // never catch a regression that let a split cross the ceiling.
    const auto stakeable = [&](CAmount value) {
        return value >= params.stakeValueRange[0] &&
               value <= params.stakeValueRange[1] &&
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

    // The upper bound the predicate now carries: a credit whose halves would
    // both exceed the ceiling must stay whole rather than fragment into two
    // outputs that equally cannot stake. Unreachable with real rewards, but it
    // is what pins the completed rule -- and what the old partial lambda missed.
    const CAmount over = 3 * params.stakeValueRange[1];
    BOOST_CHECK(!stakeable(over / 2));
    BOOST_CHECK_EQUAL(staker.SplitStakeCredit(over, SPLIT_THRESHOLD).size(), 1u);
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

/**
 * Value that is waiting and value that is stuck are different answers.
 *
 * A coin that is immature becomes stakeable with depth, and one that is too
 * young becomes stakeable with time; warning about either would be noise that
 * clears itself. The rest do not clear: under the floor, over the ceiling, on a
 * collateral amount, or at a BLS address, a coin stays out until somebody spends
 * it into a different shape, and too_old only drifts further out.
 */
BOOST_AUTO_TEST_CASE(only_the_stuck_value_counts_as_permanently_excluded)
{
    StakeSkipReport report;
    report.immature = 500 * COIN;
    report.too_young = 700 * COIN;
    BOOST_CHECK_EQUAL(PermanentlyExcluded(report), 0);
    BOOST_CHECK_EQUAL(report.Total(), 1200 * COIN);

    report.below_min = 11 * COIN;
    report.above_max = 13 * COIN;
    report.collateral = 17 * COIN;
    report.bls = 19 * COIN;
    report.too_old = 23 * COIN;
    BOOST_CHECK_EQUAL(PermanentlyExcluded(report), 83 * COIN);

    // Total still counts everything: the two answers are different questions,
    // and getstakinginfo reports the full breakdown either way.
    BOOST_CHECK_EQUAL(report.Total(), 1283 * COIN);
}

/**
 * The warning fires on an amount, not on a share.
 *
 * The case it exists for was 700,800 DFCN held permanently outside the rules by
 * a wallet carrying 567 million -- worth seventy stakes, and 0.12% of the
 * balance. A percentage threshold loose enough to catch that would fire on
 * almost anything, so the line is one whole stake's worth, taken from the
 * network's own stakeValueRange floor: under it nothing could be recovered by a
 * consolidating transaction, and at or over it something can.
 */
BOOST_AUTO_TEST_CASE(the_excluded_value_warning_triggers_on_a_recoverable_stake)
{
    const CAmount floor_value = 10000 * COIN;

    StakeSkipReport report;
    BOOST_CHECK(!ShouldWarnAboutExcludedValue(report, floor_value));

    // Waiting value never triggers it, however much of it there is.
    report.immature = 50000000 * COIN;
    report.too_young = 50000000 * COIN;
    BOOST_CHECK(!ShouldWarnAboutExcludedValue(report, floor_value));

    // A satoshi under a whole stake is still nothing anyone could act on.
    report.below_min = floor_value - 1;
    BOOST_CHECK(!ShouldWarnAboutExcludedValue(report, floor_value));

    // Exactly one stake's worth is the first amount worth reporting.
    report.below_min = floor_value;
    BOOST_CHECK(ShouldWarnAboutExcludedValue(report, floor_value));

    // The real case, at the real proportion: 0.12% of the balance, and seventy
    // stakes' worth of it.
    StakeSkipReport observed;
    observed.below_min = 70080094336983;
    BOOST_CHECK(ShouldWarnAboutExcludedValue(observed, floor_value));

    // Regtest lifts the amount rules entirely, so nothing can be excluded by
    // amount and the question does not arise.
    BOOST_CHECK(!ShouldWarnAboutExcludedValue(observed, 0));

    // The reasons are named with the same words getstakinginfo uses, so the log
    // line and the RPC cannot describe the same coins differently.
    StakeSkipReport named;
    named.below_min = 1 * COIN;
    named.collateral = 2 * COIN;
    named.immature = 99 * COIN;
    const std::string described = DescribePermanentExclusions(named);
    BOOST_CHECK(described.find("too_small") != std::string::npos);
    BOOST_CHECK(described.find("collateral_amount") != std::string::npos);
    // Waiting value is not a reason to warn, so it is not named as one either.
    BOOST_CHECK(described.find("immature") == std::string::npos);
}

/**
 * The wallet has to measure a coin's age the way the kernel does.
 *
 * CheckProofOfStake takes the time of the block being mined and subtracts the
 * time of the block holding the coin. The wallet used to subtract its own
 * bookkeeping time from the wall clock, which is a different quantity on both
 * sides. Near stakeAgeRange[0] the two disagree, and each disagreement costs
 * something in silence: either an attempt spent on a coin the kernel refuses,
 * or a coin withheld that it would have accepted.
 */
BOOST_AUTO_TEST_CASE(stake_age_is_measured_the_way_consensus_measures_it)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);

    const int64_t candidate = 2000000;
    const int64_t min_age = params.stakeAgeRange[0];
    const int64_t coin_block = candidate - min_age;   // exactly old enough

    BOOST_CHECK_EQUAL(StakeInputAge(candidate, coin_block, /*wallet_time=*/0), min_age);

    // A block time wins over the wallet's record even when the two disagree,
    // which is the entire point: the record is not the quantity consensus uses.
    const int64_t looks_brand_new = candidate;
    BOOST_CHECK_EQUAL(StakeInputAge(candidate, coin_block, looks_brand_new), min_age);

    const int deep = COINBASE_MATURITY + 2;
    const auto classify = [&](int64_t age) {
        return staker.ClassifyForStaking(100000 * COIN, deep, TxoutType::PUBKEYHASH, age,
                                         params.nPosKernelV2ActivationHeight);
    };

    // The disagreement, both ways round. Measured against its block the coin
    // qualifies and the kernel would take it; measured against a wallet record
    // that says "seen just now" it does not, and the wallet would have held
    // back a coin the network was willing to accept.
    BOOST_CHECK(classify(StakeInputAge(candidate, coin_block, looks_brand_new)) == StakeEligibility::Eligible);
    BOOST_CHECK(classify(StakeInputAge(candidate, /*coin_block_time=*/0, looks_brand_new)) == StakeEligibility::TooYoung);

    // With no block time to read there is only the wallet's record. A coin in
    // that state is unconfirmed, so the depth rule excludes it whatever the age
    // says -- the fallback exists to keep the answer defined, not to decide.
    BOOST_CHECK_EQUAL(StakeInputAge(candidate, /*coin_block_time=*/0, coin_block), min_age);
    BOOST_CHECK(staker.ClassifyForStaking(100000 * COIN, 1, TxoutType::PUBKEYHASH,
                                          StakeInputAge(candidate, 0, coin_block),
                                          params.nPosKernelV2ActivationHeight)
                == StakeEligibility::Immature);

    // A coin younger than the floor by one second is still too young, so the
    // change moves which quantity is measured and not where the line sits.
    BOOST_CHECK(classify(StakeInputAge(candidate, coin_block + 1, 0)) == StakeEligibility::TooYoung);
}

/**
 * The wallet has to hold back its own coinstake outputs, because consensus does.
 *
 * consensus/tx_verify.cpp refuses a spend of either a coinbase or a coinstake
 * under COINBASE_MATURITY. The wallet asked only about coinbases, so a staking
 * wallet counted its freshly won outputs as spendable: selection took one, the
 * transaction was built, signed and written to the wallet, and only the
 * broadcast failed -- with bad-txns-premature-spend-of-coinbase, after the
 * sender had already been shown a completed send.
 *
 * The value being tested is which transactions generate coins at all. The
 * arithmetic was never wrong.
 */
BOOST_AUTO_TEST_CASE(a_coinstake_is_immature_to_the_wallet_as_well)
{
    // Generates nothing: no wait, at any depth.
    BOOST_CHECK_EQUAL(BlocksToMaturity(false, false, 0), 0);
    BOOST_CHECK_EQUAL(BlocksToMaturity(false, false, 1), 0);
    BOOST_CHECK_EQUAL(BlocksToMaturity(false, false, COINBASE_MATURITY + 5), 0);

    // A coinbase behaves exactly as before.
    BOOST_CHECK(BlocksToMaturity(true, false, 1) > 0);
    BOOST_CHECK_EQUAL(BlocksToMaturity(true, false, COINBASE_MATURITY + 1), 0);

    // The regression, at the depth that actually produced a failed send: this
    // answered 0 -- mature -- and the coin was spent into a transaction the
    // network would not take.
    BOOST_CHECK(BlocksToMaturity(false, true, 8) > 0);

    // A coinstake matures on the same schedule as a coinbase, because the rule
    // consensus applies to the two is one rule.
    for (int depth = 0; depth <= COINBASE_MATURITY + 2; ++depth) {
        BOOST_CHECK_EQUAL(BlocksToMaturity(false, true, depth),
                          BlocksToMaturity(true, false, depth));
    }
    BOOST_CHECK_EQUAL(BlocksToMaturity(false, true, COINBASE_MATURITY + 1), 0);

    // Conflicted. A coinbase cannot reach this state, which is why the old code
    // asserted on it; an orphaned coinstake can, and nothing about a
    // transaction outside the chain is spendable.
    BOOST_CHECK(BlocksToMaturity(false, true, -1) > 0);
    BOOST_CHECK(BlocksToMaturity(true, false, -1) > 0);
}

BOOST_AUTO_TEST_SUITE_END()
