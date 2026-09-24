// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <key.h>
#include <pos/stake.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>
#include <wallet/ismine.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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
    params.stakeAgeRange = {60 * 60, 60 * 60 * 24 * 60};
    params.posTargetSpacing = 150;
    return params;
}

//! The full stakeable rule -- both bounds and the collateral exclusions.
bool Stakeable(const Consensus::Params& params, CAmount value)
{
    return value >= params.stakeValueRange[0] &&
           value <= params.stakeValueRange[1] &&
           value != params.regularMnCollateral &&
           value != params.evoMnCollateral;
}
} // namespace

/**
 * A win must never leave the winner with a coin that can no longer stake, and
 * must not walk its coins down to the floor either.
 *
 * The rule this replaced halved an output on every win from 15,000 up, so every
 * output ended between 10,000 and 20,000 and a wallet held its balance in
 * thousands of them. A split now happens only from twice the target, into equal
 * pieces of about the target. What must not change is the older guarantee:
 * below the minimum stakeable amount, above the maximum, or exactly on a
 * collateral amount, a piece is skipped from then on with nothing said.
 */
BOOST_AUTO_TEST_CASE(a_split_never_produces_an_output_that_cannot_stake)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);

    for (const CAmount target : {20000 * COIN, 50000 * COIN, 1000000 * COIN}) {
        for (CAmount credit = 10000 * COIN; credit <= 12 * target; credit += target / 37 + 13 * CENT) {
            const std::vector<CAmount> outputs = staker.SplitStakeCredit(credit, target);
            BOOST_REQUIRE(!outputs.empty());
            BOOST_CHECK(outputs.size() <= MAX_STAKE_SPLIT_OUTPUTS + 2);

            CAmount sum = 0;
            CAmount smallest = MAX_MONEY;
            CAmount largest = 0;
            for (const CAmount value : outputs) {
                sum += value;
                smallest = std::min(smallest, value);
                largest = std::max(largest, value);
            }
            BOOST_CHECK_EQUAL(sum, credit);

            // Under twice the target a credit stays whole: a piece smaller
            // than the target is exactly what the target exists to prevent.
            if (credit < 2 * target) {
                BOOST_CHECK_EQUAL(outputs.size(), 1u);
                continue;
            }
            BOOST_CHECK_MESSAGE(outputs.size() >= 2, "a credit of " << credit / COIN << " was not split at target " << target / COIN);
            for (const CAmount value : outputs) {
                BOOST_CHECK_MESSAGE(Stakeable(params, value),
                                    "splitting " << credit / COIN << " produced "
                                                 << value / COIN << ", which cannot stake");
                // About the target: never under it, never twice it.
                BOOST_CHECK(value >= target);
                BOOST_CHECK(value < 2 * target);
            }
            // Equal pieces: the first carries the rounding, less than a cent a piece.
            BOOST_CHECK(largest - smallest < static_cast<CAmount>(outputs.size()) * CENT);
        }
    }

    // Twice a collateral amount at a target of one collateral would land both
    // pieces on it; a third piece moves every piece off it.
    for (const CAmount collateral : {params.regularMnCollateral, params.evoMnCollateral}) {
        const std::vector<CAmount> outputs = staker.SplitStakeCredit(2 * collateral, collateral);
        BOOST_CHECK_EQUAL(outputs.size(), 3u);
        for (const CAmount value : outputs) {
            BOOST_CHECK(Stakeable(params, value));
        }
    }

    // One win writes at most MAX_STAKE_SPLIT_OUTPUTS pieces, however large the
    // credit is against the target; later wins keep dividing the pieces.
    BOOST_CHECK_EQUAL(staker.SplitStakeCredit(12000000 * COIN, 20000 * COIN).size(), MAX_STAKE_SPLIT_OUTPUTS);

    // A credit over the ceiling is split far enough that every piece is under
    // it, even at a target that would otherwise leave it whole.
    const CAmount over = 3 * params.stakeValueRange[1];
    BOOST_CHECK(!Stakeable(params, over));
    for (const CAmount value : staker.SplitStakeCredit(over, params.stakeValueRange[1] / 2)) {
        BOOST_CHECK(Stakeable(params, value));
    }

    // A target of zero or less never splits: the caller's guard, not a crash.
    BOOST_CHECK_EQUAL(staker.SplitStakeCredit(100000 * COIN, 0).size(), 1u);
}

/**
 * The target is sized from the network, not from the wallet.
 *
 * Whatever wins rests for about COINBASE_MATURITY blocks, and a wallet wins in
 * proportion to its share of the network's weight, so the value resting at
 * once is close to rest_blocks * target / network_weight for every wallet. A
 * target chosen as a share of the wallet's own balance -- the first design --
 * cost a two-million wallet 11 % of its reward at a 250,000 target on a
 * network of today's weight, in simulation. The automatic size keeps the
 * resting value near STAKE_REST_BUDGET_BPS instead.
 */
BOOST_AUTO_TEST_CASE(the_stake_target_follows_the_network_weight)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);
    const CAmount floor = 2 * params.stakeValueRange[0];   // 20,000
    const CAmount ceiling = params.stakeValueRange[1] / 2; // 6,250,000
    BOOST_REQUIRE_EQUAL(floor, MIN_STAKE_TARGET);

    // Nothing to go on: the floor, never a size made up from a bad estimate.
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(0, 0), floor);
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(-1, 0), floor);
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(std::nan(""), 0), floor);
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(std::numeric_limits<double>::infinity(), 0), floor);

    // The rest is the kernel's depth rule, 27 blocks, which outlasts the hour
    // of minimum age at 150 s spacing (24 blocks). One per cent of a billion
    // over 27 blocks is 370,370 whole coins.
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(1000000000.0 * COIN, 0), 370370 * COIN);
    // A network of today's measured weight sits at the floor.
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(42000000.0 * COIN, 0), floor);
    // And it cannot pass the ceiling, where a credit of just under two targets
    // would no longer be stakeable.
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(1e30, 0), ceiling);

    // Where the minimum age is the longer wait, it sets the rest instead.
    Consensus::Params slow = params;
    slow.stakeAgeRange[0] = 2 * 60 * 60; // 48 blocks at 150 s
    CStakeWallet slow_staker(nullptr, slow);
    BOOST_CHECK_EQUAL(slow_staker.StakeTargetSize(1000000000.0 * COIN, 0), 208333 * COIN);

    // A configured size wins over the network, inside the same bounds.
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(1000000000.0 * COIN, 100000 * COIN), 100000 * COIN);
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(0, 5000 * COIN), floor);
    BOOST_CHECK_EQUAL(staker.StakeTargetSize(0, MAX_MONEY), ceiling);

    // Where the network has no floor at all (regtest), MIN_STAKE_TARGET is it.
    Consensus::Params open = params;
    open.stakeValueRange = {0, MAX_MONEY};
    CStakeWallet open_staker(nullptr, open);
    BOOST_CHECK_EQUAL(open_staker.StakeTargetSize(0, 0), MIN_STAKE_TARGET);
}

/**
 * A win takes the kernel key's small outputs with it, and nothing else.
 *
 * The caller hands over only outputs this wallet can sign, paid to the
 * kernel's key, unlocked and deep enough; what is left to decide is amounts.
 * Value under the stakeable floor never stakes, so taking it costs nothing.
 * Value that stakes rests with the kernel after the win, so it is taken only
 * while the credit stays within the target.
 */
BOOST_AUTO_TEST_CASE(a_win_combines_small_outputs_within_the_target)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);
    const CAmount target = 50000 * COIN;
    const CAmount plenty = MAX_MONEY;

    const auto values_of = [](const std::vector<CAmount>& candidates, const std::vector<size_t>& chosen) {
        std::vector<CAmount> out;
        for (const size_t i : chosen) out.push_back(candidates[i]);
        return out;
    };

    // Smallest first; dust whatever the kernel's size; stakeable value only up
    // to the target; nothing at or over the target, and no collateral amount.
    const std::vector<CAmount> candidates{
        15000 * COIN,                // stakes, fits after the dust: 10k + dust + 15k <= 50k
        9 * COIN,                    // dust
        params.regularMnCollateral,  // never, whatever the target
        target,                      // not under the target
        30000 * COIN,                // stakes, would pass the target after the 15k
        1 * COIN,                    // dust
        12000 * COIN,                // stakes, fits: 10k + dust + 12k
    };
    {
        const std::vector<size_t> chosen = staker.ChooseCombineInputs(10000 * COIN, candidates, target, 19, plenty);
        const std::vector<CAmount> got = values_of(candidates, chosen);
        const std::vector<CAmount> want{1 * COIN, 9 * COIN, 12000 * COIN, 15000 * COIN};
        BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), want.begin(), want.end());
    }

    // A kernel already at the target takes only what never stakes.
    {
        const std::vector<CAmount> got = values_of(candidates, staker.ChooseCombineInputs(target, candidates, target, 19, plenty));
        const std::vector<CAmount> want{1 * COIN, 9 * COIN};
        BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), want.begin(), want.end());
    }

    // The input bound and the value bound both hold.
    BOOST_CHECK_EQUAL(staker.ChooseCombineInputs(10000 * COIN, candidates, target, 1, plenty).size(), 1u);
    BOOST_CHECK(staker.ChooseCombineInputs(10000 * COIN, candidates, target, 0, plenty).empty());
    {
        const std::vector<CAmount> got = values_of(candidates, staker.ChooseCombineInputs(10000 * COIN, candidates, target, 19, 10 * COIN));
        const std::vector<CAmount> want{1 * COIN, 9 * COIN};
        BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), want.begin(), want.end());
    }

    // A target large enough to take a collateral amount still leaves it: the
    // negative control for the exclusion, since only the target differs.
    const std::vector<CAmount> with_collateral{params.regularMnCollateral, params.regularMnCollateral + 1};
    {
        const std::vector<CAmount> got = values_of(with_collateral,
            staker.ChooseCombineInputs(10000 * COIN, with_collateral, 5000000 * COIN, 19, plenty));
        const std::vector<CAmount> want{params.regularMnCollateral + 1};
        BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), want.begin(), want.end());
    }

    // Nothing to take, nothing taken; zero and negative amounts never qualify.
    BOOST_CHECK(staker.ChooseCombineInputs(10000 * COIN, {}, target, 19, plenty).empty());
    BOOST_CHECK(staker.ChooseCombineInputs(10000 * COIN, {0, -1}, target, 19, plenty).empty());

    // The case that motivated it: pieces of exactly 10,000 at a target of
    // 20,000. Half the target is exactly such a piece, so a bound at half the
    // target -- the first draft -- combined none of them.
    const std::vector<CAmount> old_pieces(50, 10000 * COIN);
    BOOST_CHECK_EQUAL(staker.ChooseCombineInputs(10000 * COIN, old_pieces, 20000 * COIN, 19, plenty).size(), 1u);
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

/**
 * Immature value is counted exactly once, by exactly one of two mechanisms.
 *
 * Holding back immature coinstake outputs made AvailableCoins drop them before
 * ExplainExcludedCoins could classify them, so the staking report lost sight of
 * them and read 10,000 against an immature balance of 2.68 million. The report
 * now adds the wallet's immature balance to cover what the loop can no longer
 * see -- which is only correct while the two sets stay disjoint.
 *
 * They are disjoint because the thresholds sit one block apart: AvailableCoins
 * releases a generated coin at COINBASE_MATURITY + 1, and ClassifyForStaking
 * calls it Immature until COINBASE_MATURITY + 2. Move either without the other
 * and the sum double-counts a block or drops one, silently. That is what this
 * pins.
 *
 * Neither fix could have caught this alone; it lives where the two meet.
 */
BOOST_AUTO_TEST_CASE(stake_immature_accounting_has_no_gap)
{
    Consensus::Params params = MainnetLikeLimits();
    CStakeWallet staker(nullptr, params);

    // What AvailableCoins releases: a generated coin whose maturity is done.
    const auto released_for_spending = [](int depth) {
        return BlocksToMaturity(/*is_coinbase=*/false, /*is_coinstake=*/true, depth) == 0;
    };

    // What the staking classifier still holds back, on depth alone: value, age
    // and script are all chosen so nothing else can decide the answer.
    const auto immature_to_staking = [&](int depth) {
        return staker.ClassifyForStaking(100000 * COIN, depth, TxoutType::PUBKEYHASH,
                                         params.stakeAgeRange[0], params.nPosKernelV2ActivationHeight)
               == StakeEligibility::Immature;
    };

    // Below the spending threshold the loop never sees the coin, so only the
    // balance can account for it.
    for (int depth = 0; depth <= COINBASE_MATURITY; ++depth) {
        BOOST_CHECK(!released_for_spending(depth));
        BOOST_CHECK(immature_to_staking(depth));
    }

    // The sliver, exactly one block wide: released for spending, so the loop
    // counts it, and still immature for staking, so it belongs in the report.
    BOOST_CHECK(released_for_spending(COINBASE_MATURITY + 1));
    BOOST_CHECK(immature_to_staking(COINBASE_MATURITY + 1));

    // And past it, eligible -- counted by neither.
    BOOST_CHECK(released_for_spending(COINBASE_MATURITY + 2));
    BOOST_CHECK(!immature_to_staking(COINBASE_MATURITY + 2));

    // The property the addition rests on, across the whole range: a coin the
    // staking rules hold back is counted once, by the balance or by the loop
    // and never by both, and a coin they accept is counted by neither.
    for (int depth = 0; depth <= COINBASE_MATURITY + 4; ++depth) {
        const int by_balance = released_for_spending(depth) ? 0 : 1;
        const int by_loop = (released_for_spending(depth) && immature_to_staking(depth)) ? 1 : 0;
        const int expected = immature_to_staking(depth) ? 1 : 0;
        BOOST_CHECK_MESSAGE(by_balance + by_loop == expected,
                            "depth " << depth << " counted " << (by_balance + by_loop)
                                     << " times, expected " << expected);
    }
}

// The miner rested a wallet for a minute after every failed attempt, because
// the guard it consulted compared a member that no code assigns against
// COINBASE_MATURITY + 1 and was therefore always true. The outcome it treated
// as a maturity problem is the ordinary result of a staking attempt, so a
// wallet with perfectly good coins examined roughly one search time per minute
// instead of every one it was offered.
BOOST_AUTO_TEST_CASE(stake_attempt_rests_only_a_wallet_with_nothing_to_stake)
{
    // The regression. A kernel that did not win says nothing about the wallet's
    // coins, and must cost it no search times at all.
    BOOST_CHECK(!StakeAttemptWarrantsPause(StakeAttempt::NoKernelFound));

    // The one case that earns a rest: there is nothing to look for.
    BOOST_CHECK(StakeAttemptWarrantsPause(StakeAttempt::NoEligibleCoins));

    // Success is not a reason to wait, and neither is a shutdown or a fault --
    // one has its own delay after submitting, the other two want the loop to
    // reach its own exit rather than sleep first.
    BOOST_CHECK(!StakeAttemptWarrantsPause(StakeAttempt::BlockFound));
    BOOST_CHECK(!StakeAttemptWarrantsPause(StakeAttempt::Stopped));
    BOOST_CHECK(!StakeAttemptWarrantsPause(StakeAttempt::Error));
}

/**
 * A coin this wallet cannot sign must never be offered as a kernel.
 *
 * AvailableCoins lists watch-only outputs too, flagged not spendable, and the
 * selection loop used to read only value, depth, script type and age -- all of
 * which a watch-only coin can satisfy. The test builds two outputs that are
 * identical in every one of those respects and differ only in whether the
 * wallet holds the key, checks that the classifier really does accept them
 * both, and then expects exactly one of them to be selected.
 */
BOOST_FIXTURE_TEST_CASE(a_watch_only_output_is_never_offered_as_a_kernel, TestChain100Setup)
{
    Consensus::Params params = Params().GetConsensus();
    const CAmount value = 12345 * COIN; // not a collateral amount, inside the range

    // One transaction, two equal outputs: to a key this wallet will hold, and
    // to a key it will only watch. Funded from the fixture's first, long-mature
    // coinbase and mined in the next block. The wallet is not given the
    // coinbase key: selection stops as soon as it has gathered the target, and
    // the fixture's coinbases would satisfy any target before the loop reached
    // the outputs under test, so the held output must be the only spendable
    // coin the wallet has.
    CKey held;
    held.MakeNewKey(true);
    CKey watched;
    watched.MakeNewKey(true);
    const CScript spendable = GetScriptForDestination(PKHash(held.GetPubKey()));
    const CScript watch_only = GetScriptForDestination(PKHash(watched.GetPubKey()));

    CMutableTransaction funding;
    funding.vin.emplace_back(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
    funding.vout.emplace_back(value, spendable);
    funding.vout.emplace_back(value, watch_only);
    {
        FillableSigningProvider keystore;
        keystore.AddKey(coinbaseKey);
        BOOST_REQUIRE(SignSignature(keystore, *m_coinbase_txns[0], funding, 0, SIGHASH_ALL));
    }
    const uint256 funding_txid = funding.GetHash();
    CreateAndProcessBlock({funding}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    // Deep enough for the kernel's maturity rule, with room to spare.
    for (int i = 0; i < COINBASE_MATURITY + 5; ++i) {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }

    // The wallet: one key held, the other watched, then a rescan so both
    // outputs are known with their confirmations.
    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "", CreateMockWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    }
    wallet->LoadWallet();
    {
        auto* spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
        BOOST_REQUIRE(spk_man->AddKeyPubKey(held, held.GetPubKey()));
        BOOST_REQUIRE(spk_man->AddWatchOnly(watch_only, /*nCreateTime=*/0));
    }
    {
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const CWallet::ScanResult result = wallet->ScanForWalletTransactions(
            m_node.chainman->ActiveChain().Genesis()->GetBlockHash(), /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/true);
        BOOST_REQUIRE_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
    }

    // The setup is what it claims: the wallet sees both outputs, one as its own
    // and one as watched, at the same depth.
    const CWalletTx* wtx = nullptr;
    int depth = 0;
    {
        LOCK(wallet->cs_wallet);
        const auto it = wallet->mapWallet.find(funding_txid);
        BOOST_REQUIRE(it != wallet->mapWallet.end());
        wtx = &it->second;
        depth = wtx->GetDepthInMainChain();
        BOOST_CHECK(wallet->IsMine(wtx->tx->vout[0]) & ISMINE_SPENDABLE);
        BOOST_CHECK(wallet->IsMine(wtx->tx->vout[1]) & ISMINE_WATCH_ONLY);
        BOOST_CHECK(!(wallet->IsMine(wtx->tx->vout[1]) & ISMINE_SPENDABLE));
    }
    BOOST_REQUIRE(depth - 1 >= COINBASE_MATURITY + 1);

    // A candidate block an hour past the wall clock, so every coin above is
    // older than the minimum age by a wide margin; the age given to the
    // classifier below is a conservative lower bound of what selection sees.
    const int64_t nTime = GetTime() + 3600;
    const int nHeight = m_node.chainman->ActiveChain().Height() + 1;
    CStakeWallet staker(wallet, params);
    BOOST_CHECK(staker.ClassifyForStaking(value, depth, TxoutType::PUBKEYHASH, 3600 - 60, nHeight) == StakeEligibility::Eligible);

    // Every other rule accepts both outputs; only the key decides. The target
    // is more than either output alone, so neither ends the search by itself
    // and both would be taken if both were eligible.
    std::set<std::pair<const CWalletTx*, unsigned int>> chosen;
    CAmount chosen_value = 0;
    BOOST_REQUIRE(staker.SelectCoinsForStaking(2 * value, nTime, nHeight, chosen, chosen_value));
    BOOST_CHECK_EQUAL(chosen_value, value);
    BOOST_CHECK_EQUAL(chosen.count({wtx, 0}), 1U);
    BOOST_CHECK_EQUAL(chosen.count({wtx, 1}), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
