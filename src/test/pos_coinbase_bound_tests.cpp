// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <evo/chainhelper.h>
#include <governance/classes.h>
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
 * Regression coverage for F-2026-111: on a proof-of-stake block the coinbase
 * exists to carry the masternode payout and nothing else, and past the
 * coinbase-bound gate its value may not exceed the sum of the payouts the rules
 * expect (CheckPosCoinbaseValue, reached from IsBlockValueValid). The gate is
 * set on regtest (0) and devnet (7560) and unset on mainnet and testnet until
 * v23 -- so today a mainnet producer could mint a fortune into the coinbase of
 * every block it stakes, bounded by MAX_MONEY alone.
 *
 * Why this file exists although CheckPosCoinbaseValue already has tests: the
 * unit suite never reached the comparison THROUGH IsBlockValueValid. The
 * function's second gate, nHeight <= lastPowBlock, opens for every fixture
 * height (TestChain100Setup mines about a hundred blocks; regtest's
 * lastPowBlock is 5000), so the wrapper accepted any coinbase and the suite
 * stayed green whatever the bound did. Here the fixture is put past the
 * boundary for the duration of the checks, the way evo_deterministicmns_tests
 * suspends the collateral maturity.
 */
BOOST_FIXTURE_TEST_SUITE(pos_coinbase_bound_tests, TestChain100Setup)

namespace {
//! A block shaped like a proof-of-stake block: an empty-first-output coinstake
//! at vtx[1], and a coinbase at vtx[0] carrying whatever value the case wants.
CBlock MakeStakeBlock(CAmount coinstake_out, CAmount coinbase_out)
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

//! Puts the fixture's chain into the proof-of-stake regime, with the
//! coinbase-bound gate at a chosen height, for the duration of a scope.
//! Nothing is connected while the override is in force; it only changes what
//! IsBlockValueValid reads. Restores on scope exit.
struct ScopedPosRegime {
    ScopedPosRegime(Consensus::Params& params, int last_pow_block, int coinbase_bound_height) :
        m_params(params),
        m_saved_last_pow(params.lastPowBlock),
        m_saved_gate(params.nPosCoinbaseBoundActivationHeight)
    {
        m_params.lastPowBlock = last_pow_block;
        m_params.nPosCoinbaseBoundActivationHeight = coinbase_bound_height;
    }
    ~ScopedPosRegime()
    {
        m_params.lastPowBlock = m_saved_last_pow;
        m_params.nPosCoinbaseBoundActivationHeight = m_saved_gate;
    }
    Consensus::Params& m_params;
    const int m_saved_last_pow;
    const int m_saved_gate;
};

//! Makes one chosen height a superblock height for the duration of a scope:
//! CSuperblock::IsValidBlockHeight reads the start block and the cycle, and
//! with both set to the height itself that height is the first superblock.
//! Restores on scope exit.
struct ScopedSuperblockHeight {
    ScopedSuperblockHeight(Consensus::Params& params, int height) :
        m_params(params),
        m_saved_start(params.nSuperblockStartBlock),
        m_saved_cycle(params.nSuperblockCycle)
    {
        m_params.nSuperblockStartBlock = height;
        m_params.nSuperblockCycle = height;
    }
    ~ScopedSuperblockHeight()
    {
        m_params.nSuperblockStartBlock = m_saved_start;
        m_params.nSuperblockCycle = m_saved_cycle;
    }
    Consensus::Params& m_params;
    const int m_saved_start;
    const int m_saved_cycle;
};
} // namespace

BOOST_AUTO_TEST_CASE(past_the_pow_boundary_the_coinbase_may_carry_only_the_required_payouts)
{
    const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip != nullptr);
    auto& payments = *Assert(Assert(m_node.chain_helper.get())->mn_payments);
    Consensus::Params& params{const_cast<Consensus::Params&>(Params().GetConsensus())};

    const int height{tip->nHeight + 1};
    const CAmount subsidy{500 * COIN};
    const CAmount fees{0};
    const CAmount staked{12345 * COIN}; // what the kernel put in
    const CAmount fortune{100000 * COIN};
    std::string err;

    // The fixture as it stands: below lastPowBlock, so the bound does not
    // bind and a coinbase carrying a fortune passes. This is the gap this file
    // closes, stated once so the cases below are legible against it.
    BOOST_REQUIRE_LT(height, params.lastPowBlock);
    BOOST_REQUIRE_EQUAL(params.nPosCoinbaseBoundActivationHeight, 0);
    {
        CBlock block = MakeStakeBlock(staked + subsidy, fortune);
        BOOST_CHECK(payments.IsBlockValueValid(block, tip, subsidy, fees, staked, err, /*check_superblock=*/false));
    }

    // In the proof-of-stake regime with the gate active, the coinbase may
    // carry exactly what the payout rule expects. No masternode is registered
    // on this fixture and MN_RR is not active at its height, so the expected
    // payout is zero: any value in the coinbase is too much, one satoshi
    // included.
    {
        ScopedPosRegime regime(params, /*last_pow_block=*/tip->nHeight - 1, /*coinbase_bound_height=*/0);
        BOOST_REQUIRE_GT(height, params.lastPowBlock);

        CBlock empty = MakeStakeBlock(staked + subsidy, /*coinbase_out=*/0);
        BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(empty, tip, subsidy, fees, staked, err, /*check_superblock=*/false),
                            "an empty coinbase was rejected: " + err);

        CBlock one_satoshi = MakeStakeBlock(staked + subsidy, /*coinbase_out=*/1);
        BOOST_CHECK(!payments.IsBlockValueValid(one_satoshi, tip, subsidy, fees, staked, err, /*check_superblock=*/false));
        BOOST_CHECK_MESSAGE(err.find("coinbase pays too much") != std::string::npos,
                            "the rejection did not name the coinbase: " + err);

        CBlock rich = MakeStakeBlock(staked + subsidy, fortune);
        BOOST_CHECK(!payments.IsBlockValueValid(rich, tip, subsidy, fees, staked, err, /*check_superblock=*/false));

        // The coinstake's own ceiling is untouched by the coinbase rule: a
        // coinstake minting exactly the subsidy next to an empty coinbase is
        // what every block on this chain looks like.
        CBlock as_staked = MakeStakeBlock(staked + subsidy, /*coinbase_out=*/0);
        BOOST_CHECK(payments.IsBlockValueValid(as_staked, tip, subsidy, fees, staked, err, /*check_superblock=*/false));
    }

    // The gate left unset -- the state mainnet and testnet are in today, pinned
    // by pos_kernel_tests -- and the same fortune passes through the same
    // function. This is F-2026-111 reproduced through IsBlockValueValid rather
    // than through CheckPosCoinbaseValue alone. When v23 gives the gate its
    // mainnet height this block flips, and the case must be updated with it:
    // that is the regression the height release owes.
    {
        ScopedPosRegime regime(params, /*last_pow_block=*/tip->nHeight - 1, std::numeric_limits<int>::max());
        CBlock rich = MakeStakeBlock(staked + subsidy, fortune);
        BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(rich, tip, subsidy, fees, staked, err, /*check_superblock=*/false),
                            "with the gate unset the fortune was refused -- the gate is no longer the only thing "
                            "standing between a producer and the coinbase, update this case: " + err);
    }

    // Restored: the fixture is back below the boundary and the gate at 0.
    BOOST_CHECK_LT(height, params.lastPowBlock);
    BOOST_CHECK_EQUAL(params.nPosCoinbaseBoundActivationHeight, 0);
}

/**
 * At a superblock height, past the gate, the coinbase may carry at most the
 * required payouts, as at any other height. This case checks that on both
 * sides of the gate: the bound with the gate active, the inherited superblock
 * ceiling with it unset.
 */
BOOST_AUTO_TEST_CASE(at_a_superblock_height_the_bound_is_still_exactly_the_required_payouts)
{
    const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip != nullptr);
    auto& payments = *Assert(Assert(m_node.chain_helper.get())->mn_payments);
    Consensus::Params& params{const_cast<Consensus::Params&>(Params().GetConsensus())};

    const int height{tip->nHeight + 1};
    const CAmount subsidy{500 * COIN};
    const CAmount fees{0};
    const CAmount staked{12345 * COIN};
    std::string err;

    BOOST_REQUIRE(!CSuperblock::IsValidBlockHeight(height));
    const int saved_start{params.nSuperblockStartBlock};
    const int saved_cycle{params.nSuperblockCycle};
    {
        ScopedSuperblockHeight superblock(params, height);
        BOOST_REQUIRE(CSuperblock::IsValidBlockHeight(height));
        // The schedule's governance share is zero here as everywhere.
        BOOST_REQUIRE_EQUAL(CSuperblock::GetPaymentsLimit(m_node.chainman->ActiveChain(), height), 0);

        // Gate active: no masternode is registered on this fixture, so the
        // required payouts sum to zero and the coinbase must be empty. The
        // superblock height buys it nothing.
        {
            ScopedPosRegime regime(params, /*last_pow_block=*/tip->nHeight - 1, /*coinbase_bound_height=*/0);

            CBlock empty = MakeStakeBlock(staked + subsidy, /*coinbase_out=*/0);
            BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(empty, tip, subsidy, fees, staked, err, /*check_superblock=*/true),
                                "an empty coinbase was rejected at a superblock height: " + err);

            CBlock one_satoshi = MakeStakeBlock(staked + subsidy, /*coinbase_out=*/1);
            BOOST_CHECK(!payments.IsBlockValueValid(one_satoshi, tip, subsidy, fees, staked, err, /*check_superblock=*/true));
            BOOST_CHECK_MESSAGE(err.find("proof-of-stake coinbase may carry only the required payouts") != std::string::npos,
                                "the rejection did not come from the coinbase bound: " + err);
        }

        // Gate unset: the inherited superblock ceiling applies: the flat
        // masternode payment plus the block reward is accepted, one satoshi
        // more is refused.
        {
            ScopedPosRegime regime(params, /*last_pow_block=*/tip->nHeight - 1, std::numeric_limits<int>::max());
            const CAmount inherited_ceiling{GetMasternodePayment(height) + subsidy + fees};

            CBlock at_ceiling = MakeStakeBlock(staked + subsidy, inherited_ceiling);
            BOOST_CHECK_MESSAGE(payments.IsBlockValueValid(at_ceiling, tip, subsidy, fees, staked, err, /*check_superblock=*/true),
                                "with the gate unset the inherited superblock ceiling refused its own maximum: " + err);

            CBlock above_ceiling = MakeStakeBlock(staked + subsidy, inherited_ceiling + 1);
            BOOST_CHECK(!payments.IsBlockValueValid(above_ceiling, tip, subsidy, fees, staked, err, /*check_superblock=*/true));
            BOOST_CHECK_MESSAGE(err.find("exceeded superblock max value") != std::string::npos,
                                "the rejection did not come from the superblock ceiling: " + err);
        }
    }

    // Restored.
    BOOST_CHECK_EQUAL(params.nSuperblockStartBlock, saved_start);
    BOOST_CHECK_EQUAL(params.nSuperblockCycle, saved_cycle);
    BOOST_CHECK(!CSuperblock::IsValidBlockHeight(height));
}

BOOST_AUTO_TEST_SUITE_END()
