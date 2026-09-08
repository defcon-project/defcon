// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

/**
 * Regression coverage for F-2026-125.
 *
 * CTxMemPoolEntry::spendsCoinbase exists so that a rollback can re-check the
 * maturity rule -- its own comment beside the flag says so -- and this fork
 * widened the flag to cover coinstake spends as well, because the acceptance
 * path sets it for either kind of generated output. The re-check itself was
 * left as it came from upstream and asked only IsCoinBase(), so a transaction
 * spending a coinstake output that a rollback had made immature again stayed
 * in the mempool.
 *
 * Nothing downstream catches that: the block assembler performs no maturity
 * check of its own, and TestBlockValidity is skipped for proof-of-stake
 * blocks, so the producer builds a block its own node then refuses -- quietly,
 * since the minter logs the refusal under the pos category alone.
 *
 * The case does not need a staked chain. The rule reads a Coin's flags and
 * height out of the UTXO set, so the coins are placed there directly with the
 * fork's four-argument Coin constructor, at heights that are mature at the
 * current tip and immature one block below it. InvalidateBlock then performs a
 * real rollback -- the same entry point an operator's invalidateblock RPC and
 * the ChainLock conflict path both reach -- and the mempool is inspected
 * afterwards.
 *
 * Three coins make the assertion exact:
 *
 *   - a coinstake at the boundary, whose spend must now be evicted (the finding);
 *   - a coinbase at the boundary, whose spend was always evicted (no regression);
 *   - a coinstake a block deeper, whose spend must stay -- the control that
 *     proves the two removals are the maturity rule and not a blanket sweep.
 */
BOOST_FIXTURE_TEST_SUITE(pos_reorg_maturity_tests, TestChain100Setup)

namespace {
//! Place one coin in the UTXO set with the flags and height the case needs.
COutPoint PlaceCoin(CChainState& chainstate, uint32_t index, int height, bool coinbase, bool coinstake)
{
    const COutPoint outpoint{uint256::ONE, index};
    CTxOut out{50 * COIN, CScript() << OP_TRUE};
    LOCK(cs_main);
    chainstate.CoinsTip().AddCoin(outpoint, Coin(out, height, coinbase, coinstake), false);
    return outpoint;
}

//! A transaction spending one outpoint, put straight into the mempool with the
//! spends-a-generated-output flag that MemPoolAccept would have computed.
uint256 PlaceSpend(CTxMemPool& pool, const COutPoint& prevout)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = prevout;
    tx.vout.resize(1);
    tx.vout[0].nValue = 49 * COIN;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    const uint256 hash{tx.GetHash()};
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, pool.cs);
    // A real entry time matters: the default is zero, and LimitMempoolSize runs
    // right after the maturity filter -- an entry stamped 0 is expired by age and
    // would leave for a reason that has nothing to do with this case.
    pool.addUnchecked(entry.Time(GetTime()).SpendsCoinbase(true).FromTx(tx));
    return hash;
}
} // namespace

BOOST_AUTO_TEST_CASE(a_rollback_evicts_a_spend_of_a_coinstake_that_is_no_longer_mature)
{
    auto& chainman = *Assert(m_node.chainman);
    auto& chainstate = chainman.ActiveChainstate();
    auto& pool = *Assert(m_node.mempool);

    const int tip_height{WITH_LOCK(cs_main, return chainstate.m_chain.Height())};

    // Mature at the present tip by exactly one block, so a single-block
    // rollback takes them under: the rule refuses while
    // (tip + 1) - coin.nHeight < COINBASE_MATURITY.
    const int boundary_height{tip_height + 1 - COINBASE_MATURITY};
    // A block deeper, and therefore still mature after the rollback.
    const int deep_height{boundary_height - 1};
    BOOST_REQUIRE(deep_height > 0);

    const COutPoint coinstake_boundary{PlaceCoin(chainstate, 1, boundary_height, false, true)};
    const COutPoint coinbase_boundary{PlaceCoin(chainstate, 2, boundary_height, true, false)};
    const COutPoint coinstake_deep{PlaceCoin(chainstate, 3, deep_height, false, true)};

    const uint256 spend_of_coinstake_boundary{PlaceSpend(pool, coinstake_boundary)};
    const uint256 spend_of_coinbase_boundary{PlaceSpend(pool, coinbase_boundary)};
    const uint256 spend_of_coinstake_deep{PlaceSpend(pool, coinstake_deep)};
    BOOST_REQUIRE_EQUAL(pool.size(), 3U);

    // All three are spendable as things stand; nothing should have been evicted yet.
    BOOST_REQUIRE(pool.exists(spend_of_coinstake_boundary));

    // The rollback. cs_main must not be held across this call.
    {
        BlockValidationState state;
        CBlockIndex* tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip())};
        BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainstate.m_chain.Height()), tip_height - 1);

    // The finding.
    BOOST_CHECK_MESSAGE(!pool.exists(spend_of_coinstake_boundary),
                        "a spend of a coinstake the rollback made immature was left in the mempool");
    // The inherited behaviour, which must not regress.
    BOOST_CHECK_MESSAGE(!pool.exists(spend_of_coinbase_boundary),
                        "a spend of a coinbase the rollback made immature was left in the mempool");
    // The control: without it this case would pass on any code that empties the
    // mempool for reasons of its own.
    BOOST_CHECK_MESSAGE(pool.exists(spend_of_coinstake_deep),
                        "a spend of a still-mature coinstake was evicted, so the removals above "
                        "are not the maturity rule");
}

BOOST_AUTO_TEST_SUITE_END()
