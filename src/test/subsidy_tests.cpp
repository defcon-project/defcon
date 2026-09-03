// Copyright (c) 2014-2023 The Dash Core developers
// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

/**
 * What this chain pays, and what it deliberately does not.
 *
 * The suite this replaces asserted Dash's schedule: a subsidy that falls with
 * difficulty, halves every 210240 blocks, and is reallocated once v20 is
 * active. This fork removed all three. GetBlockSubsidyHelper reads one thing --
 * whether the block is past lastPowBlock -- and pays a flat 11,000,000 DFCN
 * before it and a flat 500 after, whatever the difficulty, the height or the
 * deployment says. So the old expectations were not merely stale: they
 * described economics this chain does not have, and the suite failed on every
 * build of this tree from the day the fork changed the rule.
 *
 * Written the other way round, the cases below pin what the fork does, and
 * would fail if Dash's schedule ever came back through a merge.
 */
BOOST_FIXTURE_TEST_SUITE(subsidy_tests, TestingSetup)

namespace {
//! Difficulty bits from real Dash blocks: the parameter the old schedule read,
//! and the one this fork ignores. Kept varied on purpose.
constexpr int LOW_DIFF_BITS = 0x1c4a47c4;
constexpr int HIGH_DIFF_BITS = 0x1b10cf42;

CAmount SubsidyFor(int nPrevBits, int nPrevHeight, const Consensus::Params& params, bool fV20Active = false)
{
    return GetBlockSubsidyInner(nPrevBits, nPrevHeight, params, fV20Active);
}
} // namespace

BOOST_AUTO_TEST_CASE(proof_of_work_pays_the_flat_premine_subsidy)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const CAmount pow = 11000000 * COIN;

    // The whole proof-of-work era, first block to last, at any difficulty.
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, 0, params), pow);
    BOOST_CHECK_EQUAL(SubsidyFor(HIGH_DIFF_BITS, 1, params), pow);
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, 500, params), pow);
    // nPrevHeight is the height *before* the block being paid, so the last
    // proof-of-work block is paid at nPrevHeight = lastPowBlock - 1.
    BOOST_CHECK_EQUAL(SubsidyFor(HIGH_DIFF_BITS, params.lastPowBlock - 1, params), pow);
}

BOOST_AUTO_TEST_CASE(proof_of_stake_pays_a_flat_five_hundred_forever)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const CAmount pos = 500 * COIN;

    BOOST_CHECK_EQUAL(GetProofOfStakeReward(), pos);

    // The first proof-of-stake block, and every height after it. Dash's first
    // halving (210240) and its second are included on purpose: this chain pays
    // the same at both.
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, params.lastPowBlock, params), pos);
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, params.lastPowBlock + 1, params), pos);
    BOOST_CHECK_EQUAL(SubsidyFor(HIGH_DIFF_BITS, 99999, params), pos);
    BOOST_CHECK_EQUAL(SubsidyFor(HIGH_DIFF_BITS, params.nSubsidyHalvingInterval, params), pos);
    BOOST_CHECK_EQUAL(SubsidyFor(HIGH_DIFF_BITS, 2 * params.nSubsidyHalvingInterval, params), pos);
    BOOST_CHECK_EQUAL(SubsidyFor(HIGH_DIFF_BITS, 100 * params.nSubsidyHalvingInterval, params), pos);
}

BOOST_AUTO_TEST_CASE(the_boundary_is_the_only_thing_that_moves_the_subsidy)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();

    // One block apart, across lastPowBlock: the only step this schedule has.
    const CAmount last_pow = SubsidyFor(LOW_DIFF_BITS, params.lastPowBlock - 1, params);
    const CAmount first_pos = SubsidyFor(LOW_DIFF_BITS, params.lastPowBlock, params);
    BOOST_CHECK_EQUAL(last_pow, 11000000 * COIN);
    BOOST_CHECK_EQUAL(first_pos, 500 * COIN);
    BOOST_CHECK(last_pow != first_pos);

    // Difficulty is read by Dash's schedule and by nothing here: same height,
    // wildly different bits, same amount, on both sides of the boundary.
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, 500, params), SubsidyFor(HIGH_DIFF_BITS, 500, params));
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, 500000, params), SubsidyFor(HIGH_DIFF_BITS, 500000, params));

    // v20 reallocation likewise: the flag exists in the signature and changes
    // nothing, before or after the boundary.
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, 500, params, /*fV20Active=*/false),
                      SubsidyFor(LOW_DIFF_BITS, 500, params, /*fV20Active=*/true));
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, 500000, params, /*fV20Active=*/false),
                      SubsidyFor(LOW_DIFF_BITS, 500000, params, /*fV20Active=*/true));

    // And nothing is ever carved out for a superblock.
    BOOST_CHECK_EQUAL(GetSuperblockSubsidyInner(LOW_DIFF_BITS, 500, params, /*fV20Active=*/false), 0);
    BOOST_CHECK_EQUAL(GetSuperblockSubsidyInner(HIGH_DIFF_BITS, 500000, params, /*fV20Active=*/true), 0);
}

BOOST_AUTO_TEST_CASE(the_masternode_payment_is_flat)
{
    // 10,000 DFCN per paid block, at every height and whatever the block is
    // worth. Dash scales this with the subsidy; this fork does not, which is
    // why a proof-of-stake block mints 10,500 and not 500.
    const CAmount payment = 10000 * COIN;
    BOOST_CHECK_EQUAL(GetMasternodePayment(1), payment);
    BOOST_CHECK_EQUAL(GetMasternodePayment(1000), payment);
    BOOST_CHECK_EQUAL(GetMasternodePayment(1000000), payment);
    BOOST_CHECK_EQUAL(GetMasternodePayment(1000, 11000000 * COIN, /*fV20Active=*/false), payment);
    BOOST_CHECK_EQUAL(GetMasternodePayment(1000, 500 * COIN, /*fV20Active=*/true), payment);
}

BOOST_AUTO_TEST_CASE(every_network_pays_by_its_own_boundary)
{
    // The rule is one comparison against lastPowBlock, so the schedule is the
    // same everywhere and only the height of the step differs. Pinned here
    // because a network whose boundary moved without its genesis moving would
    // pay 11,000,000 for blocks the chain had already paid 500 for.
    struct Net {
        std::string name;
        int lastPowBlock;
    };
    const std::vector<Net> nets{
        {CBaseChainParams::MAIN, 999},
        {CBaseChainParams::TESTNET, 999},
        {CBaseChainParams::REGTEST, 5000},
    };
    for (const auto& net : nets) {
        const auto chainParams = CreateChainParams(*m_node.args, net.name);
        const auto& params = chainParams->GetConsensus();
        BOOST_CHECK_EQUAL(params.lastPowBlock, net.lastPowBlock);
        BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, params.lastPowBlock - 1, params), 11000000 * COIN);
        BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, params.lastPowBlock, params), 500 * COIN);
    }

    gArgs.SoftSetBoolArg("-devnet", true);
    const auto devnet = CreateChainParams(*m_node.args, CBaseChainParams::DEVNET);
    const auto& dparams = devnet->GetConsensus();
    BOOST_CHECK_EQUAL(dparams.lastPowBlock, 999);
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, dparams.lastPowBlock - 1, dparams), 11000000 * COIN);
    BOOST_CHECK_EQUAL(SubsidyFor(LOW_DIFF_BITS, dparams.lastPowBlock, dparams), 500 * COIN);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_SUITE_END()
