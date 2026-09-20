// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/params.h>
#include <governance/governance.h>
#include <spork.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>

/**
 * Superblocks are retired at nSuperblocksRetiredHeight, the ninth height of
 * the v23 bundle. AreSuperblocksEnabled answers from two things: the spork,
 * exactly as before, below the height; and no from the height on.
 *
 * The cases ask it with the height unset, with the bundle applied, below and
 * at a height set by hand, and with the devnet's value.
 */
BOOST_AUTO_TEST_SUITE(superblock_retirement_tests)

namespace {
constexpr int UNSET = std::numeric_limits<int>::max();
constexpr int H = 168000; // on the Q60 grid, as the release requires

struct MainNetSetup : public BasicTestingSetup {
    MainNetSetup() : BasicTestingSetup(CBaseChainParams::MAIN) {}
};

struct RegTestSetup : public BasicTestingSetup {
    RegTestSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};

struct DevNetSetup : public BasicTestingSetup {
    DevNetSetup() : BasicTestingSetup(CBaseChainParams::DEVNET, {"-devnet=superblock-retirement-tests", "-listen=0"}) {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(the_bundle_retires_superblocks_at_its_height, MainNetSetup)
{
    const CSporkManager sporkman;
    const bool spork{sporkman.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)};

    Consensus::Params c = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(c.nSuperblocksRetiredHeight, UNSET);
    // Dormant: at every height the answer is the spork's.
    for (const int height : {1, 1000, 100000, 10000000}) {
        BOOST_CHECK_MESSAGE(AreSuperblocksEnabled(sporkman, height, c) == spork,
                            "the answer at " + std::to_string(height) + " is not the spork's, with no height set");
    }

    // Scheduled: the spork still answers below H, and nothing does from H on.
    ApplyV23ActivationBundle(c, H);
    BOOST_REQUIRE_EQUAL(c.nSuperblocksRetiredHeight, H);
    BOOST_CHECK_EQUAL(AreSuperblocksEnabled(sporkman, H - 1, c), spork);
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, H, c));
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, H + 100000, c));
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, UNSET - 1, c));
}

BOOST_FIXTURE_TEST_CASE(below_the_height_the_spork_decides, RegTestSetup)
{
    // A fresh manager on a test chain holds the spork's default: off. With the
    // height unset that is the whole answer -- the retirement adds nothing
    // below its height, it only takes the spork's yes away from the height on.
    const CSporkManager sporkman;
    BOOST_REQUIRE(!sporkman.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED));
    Consensus::Params c = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(c.nSuperblocksRetiredHeight, UNSET);
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, 1500, c));
    c.nSuperblocksRetiredHeight = 2000;
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, 1500, c));
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, 2000, c));
}

BOOST_FIXTURE_TEST_CASE(devnet_is_retired_from_genesis, DevNetSetup)
{
    const Consensus::Params& c = Params().GetConsensus();
    BOOST_CHECK_EQUAL(c.nSuperblocksRetiredHeight, 0);
    const CSporkManager sporkman;
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, 0, c));
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, 100000, c));
}

BOOST_AUTO_TEST_SUITE_END()
