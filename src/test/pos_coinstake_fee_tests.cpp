// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <evo/chainhelper.h>
#include <masternode/payments.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
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

BOOST_AUTO_TEST_CASE(fee_burn_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosFeeBurnActivationHeight,
                      std::numeric_limits<int>::max());
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
