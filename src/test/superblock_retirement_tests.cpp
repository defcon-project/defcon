// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <evo/chainhelper.h>
#include <governance/classes.h>
#include <governance/governance.h>
#include <key.h>
#include <key_io.h>
#include <masternode/payments.h>
#include <masternode/sync.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <spork.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <string>

/**
 * Superblocks are retired at nSuperblocksRetiredHeight, the ninth height of
 * the v23 bundle. AreSuperblocksEnabled answers from two things: the spork,
 * exactly as before, below the height; and no from the height on.
 *
 * The cases ask it with the height unset, with the bundle applied, below and
 * at a height set by hand, and with the devnet's value; then on a test chain
 * with the spork on, at the predicate and through IsBlockValueValid; and the
 * last one reads the devnet's superblock schedule against its gate heights.
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

// The state no case above reaches: a test chain with the spork switched ON and
// the height set by hand -- first at the predicate, then through the payments
// code, which has to hand the predicate the height of the block it is judging.
BOOST_FIXTURE_TEST_CASE(a_test_chain_with_the_spork_on, TestChain100Setup)
{
    CKey key;
    key.MakeNewKey(false);
    CSporkManager& sporkman = *Assert(m_node.sporkman.get());
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash(key.GetPubKey()))));
    BOOST_REQUIRE(sporkman.SetMinSporkKeys(1));
    BOOST_REQUIRE(sporkman.SetPrivKey(EncodeSecret(key)));
    BOOST_REQUIRE(!sporkman.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED));
    BOOST_REQUIRE(sporkman.UpdateSpork(*Assert(m_node.peerman.get()), SPORK_9_SUPERBLOCKS_ENABLED, 0));
    BOOST_REQUIRE(sporkman.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED));
    // The control that the predicate reads THIS spork: another one stays off.
    BOOST_REQUIRE(!sporkman.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED));

    Consensus::Params& params{const_cast<Consensus::Params&>(Params().GetConsensus())};
    BOOST_REQUIRE_EQUAL(params.nSuperblocksRetiredHeight, UNSET);
    const int saved_start{params.nSuperblockStartBlock};
    const int saved_cycle{params.nSuperblockCycle};

    params.nSuperblocksRetiredHeight = 2000;
    BOOST_CHECK(AreSuperblocksEnabled(sporkman, 1999, params));
    BOOST_CHECK(!AreSuperblocksEnabled(sporkman, 2000, params));
    params.nSuperblocksRetiredHeight = UNSET;
    BOOST_CHECK(AreSuperblocksEnabled(sporkman, 2000, params));

    // IsBlockValueValid only reaches the predicate on a node that has
    // finished its masternode sync.
    BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(false));
    BOOST_REQUIRE(m_node.govman->LoadCache(false));
    for (int i = 0; i < 4 && !m_node.mn_sync->IsSynced(); ++i) m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());
    BOOST_REQUIRE(m_node.govman->IsValid());

    const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip != nullptr);
    auto& payments = *Assert(Assert(m_node.chain_helper.get())->mn_payments);
    const int height{tip->nHeight + 1};
    params.nSuperblockStartBlock = height;
    params.nSuperblockCycle = height;
    BOOST_REQUIRE(CSuperblock::IsValidBlockHeight(height));

    // A proof-of-work block whose coinbase is one coin over the block reward.
    // Whether it is accepted follows the predicate's answer, so the three
    // calls below read the height IsBlockValueValid hands the predicate.
    const CAmount subsidy{500 * COIN};
    const CAmount fees{0};
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = subsidy + 1 * COIN;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    BOOST_REQUIRE(block.IsProofOfWork());
    std::string err;

    BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(block, tip, subsidy, fees, 0, err, /*check_superblock=*/false),
                        "height unset, spork on: " + err);
    params.nSuperblocksRetiredHeight = height;
    BOOST_CHECK(!payments.IsBlockValueValid(block, tip, subsidy, fees, 0, err, /*check_superblock=*/false));
    BOOST_CHECK_MESSAGE(err.find("superblocks are disabled") != std::string::npos, "retired at this height: " + err);
    params.nSuperblocksRetiredHeight = height + 1;
    BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(block, tip, subsidy, fees, 0, err, /*check_superblock=*/false),
                        "retired one block later: " + err);

    params.nSuperblocksRetiredHeight = UNSET;
    params.nSuperblockStartBlock = saved_start;
    params.nSuperblockCycle = saved_cycle;
}

// Parameter consistency on the devnet, whose heights are set one by one and
// not by the bundle: the first height of the superblock schedule from
// nSuperblocksRetiredHeight on, read against nPosCoinbaseBoundActivationHeight.
// The bundle writes one height into both, and chainparams_v23_bundle_tests
// holds that.
BOOST_FIXTURE_TEST_CASE(superblock_schedule_vs_gate_heights_on_devnet, DevNetSetup)
{
    const Consensus::Params& c = Params().GetConsensus();
    BOOST_REQUIRE(c.nSuperblocksRetiredHeight != UNSET);
    const int cycle{c.nSuperblockCycle};
    int first{std::max(c.nSuperblockStartBlock, c.nSuperblocksRetiredHeight)};
    if (first % cycle != 0) first += cycle - first % cycle;
    BOOST_REQUIRE(CSuperblock::IsValidBlockHeight(first));
    BOOST_CHECK_MESSAGE(first >= c.nPosCoinbaseBoundActivationHeight,
                        "devnet: first scheduled height " + std::to_string(first) + ", nSuperblocksRetiredHeight " +
                            std::to_string(c.nSuperblocksRetiredHeight) + ", nPosCoinbaseBoundActivationHeight " +
                            std::to_string(c.nPosCoinbaseBoundActivationHeight));
}

BOOST_AUTO_TEST_SUITE_END()
