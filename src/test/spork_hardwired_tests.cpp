// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <governance/classes.h>
#include <governance/governance.h>
#include <spork.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>

/**
 * On mainnet every spork is hardwired: GetSporkValue answers from a table and
 * never from the network. All of them are on -- except the superblock spork.
 *
 * Governance has no budget on this chain. GetBlockSubsidyHelper gives
 * superblocks a zero share of the subsidy, so CSuperblock::GetPaymentsLimit is
 * zero and a superblock can never pay. With nothing to pay there is nothing to
 * enable: with the spork off, block production never adds superblock outputs
 * and validation never consults a trigger, so every block is held to the
 * ordinary reward limits and nothing else.
 *
 * The first case pins the table, the second the reason for its one exception,
 * and the third is the control that the first measures the table at all: on a
 * test chain the same manager, with no spork message received, answers from
 * the definitions' defaults, and every spork is off.
 */
BOOST_AUTO_TEST_SUITE(spork_hardwired_tests)

namespace {
struct MainNetSetup : public BasicTestingSetup {
    MainNetSetup() : BasicTestingSetup(CBaseChainParams::MAIN) {}
};

struct RegTestSetup : public BasicTestingSetup {
    RegTestSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(mainnet_hardwires_every_spork_on_except_superblocks, MainNetSetup)
{
    BOOST_REQUIRE(!Params().IsTestChain());
    const CSporkManager sporkman;

    for (const auto& def : sporkDefs) {
        const bool active{sporkman.IsSporkActive(def.sporkId)};
        if (def.sporkId == SPORK_9_SUPERBLOCKS_ENABLED) {
            BOOST_CHECK_MESSAGE(!active, "superblocks are enabled on mainnet, where governance has nothing to pay with");
        } else {
            BOOST_CHECK_MESSAGE(active, std::string{def.name} + " is not active on mainnet");
        }
    }

    // The two values that are not the plain "on".
    BOOST_CHECK_EQUAL(sporkman.GetSporkValue(SPORK_21_QUORUM_ALL_CONNECTED), 1);
    BOOST_CHECK_EQUAL(sporkman.GetSporkValue(SPORK_9_SUPERBLOCKS_ENABLED), 4070908800LL);

    // What block production and validation actually ask.
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman));
}

BOOST_FIXTURE_TEST_CASE(the_exception_stands_on_a_zero_governance_budget, MainNetSetup)
{
    const Consensus::Params& consensus{Params().GetConsensus()};
    int height{consensus.nSuperblockStartBlock};
    if (height % consensus.nSuperblockCycle != 0) height += consensus.nSuperblockCycle - height % consensus.nSuperblockCycle;
    // Off a superblock height the limit is zero before the subsidy is read;
    // asking there would prove nothing.
    BOOST_REQUIRE(CSuperblock::IsValidBlockHeight(height));

    // GetPaymentsLimit takes the active chain and reads the parameters alone.
    const CChain chain;
    for (int i = 0; i < 5; ++i) {
        BOOST_CHECK_MESSAGE(CSuperblock::GetPaymentsLimit(chain, height + i * consensus.nSuperblockCycle) == 0,
                            "governance has a budget on mainnet: a superblock can pay, so SPORK_9_SUPERBLOCKS_ENABLED "
                            "has to be hardwired on again in the same change");
    }
}

BOOST_FIXTURE_TEST_CASE(a_test_chain_answers_from_the_defaults_and_not_from_the_table, RegTestSetup)
{
    BOOST_REQUIRE(Params().IsTestChain());
    const CSporkManager sporkman;

    // No spork message has reached this manager, so each spork carries its
    // definition's default -- off, all seven. On mainnet the same call says on
    // for six of them: that difference is the table.
    for (const auto& def : sporkDefs) {
        BOOST_CHECK_MESSAGE(!sporkman.IsSporkActive(def.sporkId), std::string{def.name} + " is active on a fresh test-chain manager");
        BOOST_CHECK_EQUAL(sporkman.GetSporkValue(def.sporkId), def.defaultValue);
    }
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman));
}

BOOST_AUTO_TEST_SUITE_END()
