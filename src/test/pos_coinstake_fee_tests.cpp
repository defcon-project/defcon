// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
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
#include <tinyformat.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <string>

/**
 * The transaction fees of a proof-of-stake block are destroyed, by rule.
 *
 * They were destroyed before this too, but only because our own wallet never
 * asked for them: the ceiling on a coinstake was the subsidy plus the fees, so
 * a producer running modified software could have kept them and every node
 * would have accepted the block. An economic rule enforced by nothing but the
 * software that happens to be running is a rule anybody may rewrite, which is
 * why this is a consensus check and not a comment.
 *
 * Nothing on the chain changes: every one of the 5896 proof-of-stake blocks
 * mined before this rule minted exactly the subsidy.
 */
BOOST_FIXTURE_TEST_SUITE(pos_coinstake_fee_tests, TestChain100Setup)

namespace {
//! The release height: the one place this file names it. It is V23_MAINNET_ACTIVATION_HEIGHT in
//! chainparams.cpp, and moving the release height moves both. Testnet stays unscheduled.
constexpr int RELEASE_H = 144888;

//! A block shaped like a proof-of-stake block: an empty-first-output coinstake
//! at vtx[1], and a coinbase at vtx[0] carrying the masternode payout.
CBlock MakeStakeBlock(CAmount coinstake_out, CAmount coinbase_out = 0)
{
    CBlock block;

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = coinbase_out;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;

    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vin[0].prevout = COutPoint(uint256S("0x01"), 0);
    coinstake.vout.resize(2);
    coinstake.vout[0].SetEmpty();
    coinstake.vout[1].nValue = coinstake_out;
    coinstake.vout[1].scriptPubKey = CScript() << OP_TRUE;

    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.vtx.push_back(MakeTransactionRef(std::move(coinstake)));
    return block;
}
} // namespace

BOOST_AUTO_TEST_CASE(past_the_gate_the_coinstake_may_not_keep_the_fees)
{
    const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip != nullptr);
    auto& payments = *Assert(Assert(m_node.chain_helper.get())->mn_payments);

    const CAmount subsidy{500 * COIN};
    const CAmount fees{7277};           // a real block's fees, from the devnet
    const CAmount staked{12345 * COIN}; // what the kernel put in
    const int height{tip->nHeight + 1};

    // Regtest activates the rule at height 0, so the fixture's chain is already
    // past the gate. Read it rather than assume it.
    BOOST_REQUIRE(PosFeesAreBurned(height, Params().GetConsensus()));

    std::string err;

    // The subsidy alone: what the wallet mints, and what every block on the
    // chain has always minted. Still accepted, which is the whole point -- the
    // rule invalidates nothing that exists.
    {
        CBlock block = MakeStakeBlock(staked + subsidy);
        BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false),
                            "the subsidy alone was rejected: " + err);
    }

    // Keeping the fees: refused past the gate. Before it, the same block was
    // valid, and that is the hole this closes.
    {
        CBlock block = MakeStakeBlock(staked + subsidy + fees);
        BOOST_CHECK_MESSAGE(!payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false),
                            "a coinstake that kept the fees was accepted");
        BOOST_CHECK_MESSAGE(err.find("coinstake mints too much") != std::string::npos,
                            "the rejection did not name the coinstake: " + err);
    }

    // One satoshi over the subsidy is over, fees or no fees.
    {
        CBlock block = MakeStakeBlock(staked + subsidy + 1);
        BOOST_CHECK(!payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false));
        BOOST_CHECK(!payments.IsBlockValueValid(block, tip, subsidy, /*feeReward=*/0, staked, err, /*check_superblock=*/false));
    }

    // And under the subsidy is still allowed: the rule is a ceiling, not an
    // amount. A producer that wants to mint less may.
    {
        CBlock block = MakeStakeBlock(staked + subsidy - 1);
        BOOST_CHECK(payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false));
    }
}

// The same wording at a superblock height. IsBlockValueValid falls back to the
// block reward limits there in two places -- superblocks disabled, and enabled
// with no superblock triggered -- and both report a proof-of-stake block by
// its coinstake and the coinstake's own ceiling, as the ordinary-height branch
// above does, and a proof-of-work block by its coinbase and the block reward.
BOOST_AUTO_TEST_CASE(at_a_superblock_height_the_rejection_names_the_coinstake_too)
{
    const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip != nullptr);
    auto& payments = *Assert(Assert(m_node.chain_helper.get())->mn_payments);
    Consensus::Params& params{const_cast<Consensus::Params&>(Params().GetConsensus())};
    CSporkManager& sporkman = *Assert(m_node.sporkman.get());

    const CAmount subsidy{500 * COIN};
    const CAmount fees{7277};
    const CAmount staked{12345 * COIN};
    const int height{tip->nHeight + 1};
    // Past the fee gate the coinstake's ceiling is the subsidy and the block
    // reward is the subsidy plus the fees: the two limits differ, so a message
    // that printed the wrong one would show.
    BOOST_REQUIRE(PosFeesAreBurned(height, params));

    // Both fallbacks sit behind the masternode sync.
    BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(false));
    BOOST_REQUIRE(m_node.govman->LoadCache(false));
    for (int i = 0; i < 4 && !m_node.mn_sync->IsSynced(); ++i) m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());
    BOOST_REQUIRE(m_node.govman->IsValid());

    // The next height made a superblock height for the rest of the case.
    struct ScopedSuperblockHeight {
        ScopedSuperblockHeight(Consensus::Params& p, int h) : m_p(p), m_start(p.nSuperblockStartBlock), m_cycle(p.nSuperblockCycle)
        {
            m_p.nSuperblockStartBlock = h;
            m_p.nSuperblockCycle = h;
        }
        ~ScopedSuperblockHeight()
        {
            m_p.nSuperblockStartBlock = m_start;
            m_p.nSuperblockCycle = m_cycle;
        }
        Consensus::Params& m_p;
        const int m_start;
        const int m_cycle;
    } superblock_height(params, height);
    BOOST_REQUIRE(CSuperblock::IsValidBlockHeight(height));

    const CBlock stake_at_ceiling = MakeStakeBlock(staked + subsidy);
    const CBlock stake_over = MakeStakeBlock(staked + subsidy + 1);
    CBlock work_over;
    {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vout.resize(1);
        coinbase.vout[0].nValue = subsidy + fees + 1;
        coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
        work_over.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    }
    BOOST_REQUIRE(stake_over.IsProofOfStake());
    BOOST_REQUIRE(work_over.IsProofOfWork());
    const std::string stake_numbers{strprintf("at height %d (actual=%d vs limit=%d)", height, subsidy + 1, subsidy)};
    const std::string work_numbers{strprintf("at height %d (actual=%d vs limit=%d)", height, subsidy + fees + 1, subsidy + fees)};

    const auto check_wording = [&](const std::string& branch) {
        std::string err;
        BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(stake_at_ceiling, tip, subsidy, fees, staked, err, /*check_superblock=*/true),
                            branch + ": a coinstake at its ceiling was rejected: " + err);

        BOOST_CHECK(!payments.IsBlockValueValid(stake_over, tip, subsidy, fees, staked, err, /*check_superblock=*/true));
        const std::string stake_err{err};
        BOOST_CHECK_MESSAGE(stake_err.find("coinstake mints too much " + stake_numbers) != std::string::npos,
                            branch + ": the rejection did not report the coinstake and its ceiling: " + stake_err);
        BOOST_CHECK_MESSAGE(stake_err.find("coinbase pays") == std::string::npos,
                            branch + ": the rejection of a proof-of-stake block named the coinbase: " + stake_err);
        BOOST_CHECK_MESSAGE(stake_err.find(branch) != std::string::npos, "not the " + branch + " fallback: " + stake_err);

        BOOST_CHECK(!payments.IsBlockValueValid(work_over, tip, subsidy, fees, 0, err, /*check_superblock=*/true));
        const std::string work_err{err};
        BOOST_CHECK_MESSAGE(work_err.find("coinbase pays too much " + work_numbers) != std::string::npos,
                            branch + ": the rejection did not report the coinbase and the block reward: " + work_err);
        BOOST_CHECK_MESSAGE(work_err.find(branch) != std::string::npos, "not the " + branch + " fallback: " + work_err);
    };

    // A fresh spork manager on a test chain holds the spork's default: off.
    BOOST_REQUIRE(!AreSuperblocksEnabled(sporkman, height, params));
    check_wording("superblocks are disabled");

    // The spork on, and nothing in the governance store to trigger a superblock.
    CKey key;
    key.MakeNewKey(false);
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash(key.GetPubKey()))));
    BOOST_REQUIRE(sporkman.SetMinSporkKeys(1));
    BOOST_REQUIRE(sporkman.SetPrivKey(EncodeSecret(key)));
    BOOST_REQUIRE(sporkman.UpdateSpork(*Assert(m_node.peerman.get()), SPORK_9_SUPERBLOCKS_ENABLED, 0));
    BOOST_REQUIRE(AreSuperblocksEnabled(sporkman, height, params));
    check_wording("no triggered superblock detected");
}

BOOST_AUTO_TEST_CASE(below_the_gate_the_old_ceiling_stands)
{
    // The rule is height-gated, and below the gate the ceiling is what it has
    // always been: subsidy plus fees. Checked through the decision itself,
    // because a fixture cannot put the chain on the other side of height 0.
    Consensus::Params p;
    p.nPosFeeBurnActivationHeight = 7920;
    BOOST_CHECK(!PosFeesAreBurned(7919, p));
    BOOST_CHECK(PosFeesAreBurned(7920, p));
    BOOST_CHECK(PosFeesAreBurned(100000, p));

    p.nPosFeeBurnActivationHeight = std::numeric_limits<int>::max();
    BOOST_CHECK(!PosFeesAreBurned(100000, p));

    p.nPosFeeBurnActivationHeight = 0;
    BOOST_CHECK(PosFeesAreBurned(0, p));
}

BOOST_AUTO_TEST_CASE(below_the_gate_the_coinstake_may_keep_the_fees)
{
    // The case above reads the decision alone. This one goes through
    // IsBlockValueValid, as the case past the gate does: the chain cannot be
    // moved below height 0, so the gate is moved above the next height instead,
    // for the length of the case.
    const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip != nullptr);
    auto& payments = *Assert(Assert(m_node.chain_helper.get())->mn_payments);
    Consensus::Params& params{const_cast<Consensus::Params&>(Params().GetConsensus())};

    const CAmount subsidy{500 * COIN};
    const CAmount fees{7277};
    const CAmount staked{12345 * COIN};
    const int height{tip->nHeight + 1};

    struct ScopedFeeBurnGate {
        ScopedFeeBurnGate(Consensus::Params& p, int h) : m_p(p), m_saved(p.nPosFeeBurnActivationHeight)
        {
            m_p.nPosFeeBurnActivationHeight = h;
        }
        ~ScopedFeeBurnGate() { m_p.nPosFeeBurnActivationHeight = m_saved; }
        Consensus::Params& m_p;
        const int m_saved;
    } gate(params, height + 1);
    BOOST_REQUIRE(!PosFeesAreBurned(height, params));

    std::string err;

    // Keeping the fees: valid below the gate, which is what the chain allowed
    // until now.
    {
        CBlock block = MakeStakeBlock(staked + subsidy + fees);
        BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false),
                            "below the gate a coinstake that kept the fees was rejected: " + err);
    }

    // The subsidy alone, as the wallet mints it, is valid on both sides.
    {
        CBlock block = MakeStakeBlock(staked + subsidy);
        BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false),
                            "below the gate the subsidy alone was rejected: " + err);
    }

    // The old ceiling is still a ceiling: one satoshi over subsidy plus fees.
    {
        CBlock block = MakeStakeBlock(staked + subsidy + fees + 1);
        BOOST_CHECK(!payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false));
        BOOST_CHECK_MESSAGE(err.find("coinstake mints too much") != std::string::npos,
                            "the rejection did not name the coinstake: " + err);
    }
}

BOOST_AUTO_TEST_CASE(fee_burn_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosFeeBurnActivationHeight,
                      RELEASE_H);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nPosFeeBurnActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nPosFeeBurnActivationHeight,
                      0);
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nPosFeeBurnActivationHeight,
                      7920);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_SUITE_END()
