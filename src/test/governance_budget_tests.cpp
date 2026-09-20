// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <governance/classes.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <vector>

/**
 * The reward schedule of this chain has no governance share:
 * GetBlockSubsidyHelper returns a zero superblock share on every network, so
 * CSuperblock::GetPaymentsLimit is zero at every superblock height. These
 * cases pin that.
 */
BOOST_AUTO_TEST_SUITE(governance_budget_tests)

namespace {
struct MainNetSetup : public BasicTestingSetup {
    MainNetSetup() : BasicTestingSetup(CBaseChainParams::MAIN) {}
};

struct TestNetSetup : public BasicTestingSetup {
    TestNetSetup() : BasicTestingSetup(CBaseChainParams::TESTNET) {}
};

struct DevNetSetup : public BasicTestingSetup {
    DevNetSetup() : BasicTestingSetup(CBaseChainParams::DEVNET, {"-devnet=governance-budget-tests", "-listen=0"}) {}
};

struct RegTestSetup : public BasicTestingSetup {
    RegTestSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};

//! The first five superblock heights of the selected network and one far
//! ahead. On regtest the first five sit below the proof-of-work boundary and
//! the far one above it; on the other networks every superblock height is a
//! proof-of-stake height.
std::vector<int> SuperblockHeights()
{
    const Consensus::Params& consensus{Params().GetConsensus()};
    const int cycle{consensus.nSuperblockCycle};
    int first{consensus.nSuperblockStartBlock};
    if (first % cycle != 0) first += cycle - first % cycle;

    std::vector<int> heights;
    for (int i = 0; i < 5; ++i) heights.push_back(first + i * cycle);
    heights.push_back(first + 1000 * cycle);
    return heights;
}

void CheckTheBudgetIsZero()
{
    const Consensus::Params& consensus{Params().GetConsensus()};
    // GetPaymentsLimit takes the active chain and reads the parameters alone.
    const CChain chain;

    for (const int height : SuperblockHeights()) {
        // The control that the limit below is the COMPUTED one: off a
        // superblock height GetPaymentsLimit returns zero before it looks at
        // the subsidy at all, and a test that only ever asked there would pass
        // whatever the governance share was.
        BOOST_REQUIRE_MESSAGE(CSuperblock::IsValidBlockHeight(height), strprintf("%d is not a superblock height", height));
        BOOST_CHECK_MESSAGE(CSuperblock::GetPaymentsLimit(chain, height) == 0,
                            strprintf("the governance payments limit at height %d is not zero", height));
        // The share itself, on both sides of the V20 switch the helper is handed.
        for (const bool v20_active : {false, true}) {
            BOOST_CHECK_EQUAL(GetSuperblockSubsidyInner(/*nPrevBits=*/1, height - 1, consensus, v20_active), 0);
        }
    }
    BOOST_CHECK(!CSuperblock::IsValidBlockHeight(SuperblockHeights().front() + 1));
}
} // namespace

BOOST_FIXTURE_TEST_CASE(the_budget_is_zero_on_mainnet, MainNetSetup) { CheckTheBudgetIsZero(); }
BOOST_FIXTURE_TEST_CASE(the_budget_is_zero_on_testnet, TestNetSetup) { CheckTheBudgetIsZero(); }
BOOST_FIXTURE_TEST_CASE(the_budget_is_zero_on_devnet, DevNetSetup) { CheckTheBudgetIsZero(); }
BOOST_FIXTURE_TEST_CASE(the_budget_is_zero_on_regtest, RegTestSetup) { CheckTheBudgetIsZero(); }

BOOST_AUTO_TEST_SUITE_END()
