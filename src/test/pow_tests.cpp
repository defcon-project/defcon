// Copyright (c) 2015-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/setup_common.h>

#include <list>
#include <string>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

namespace {
//! A chain of index entries with real heights and a chosen spacing.
//!
//! The suite this replaces built one without heights, because Dash's DGW walks
//! by pprev and never asks for a height. This fork replaced DGW with LWMA3,
//! which walks with GetAncestor(), so the same chain sent it off the front and
//! the assertion inside GetAncestor took the whole test binary down -- taking
//! every suite after pow_tests with it.
std::list<CBlockIndex> MakeChain(int tip_height, size_t count, int64_t spacing, uint32_t bits, int64_t start_time = 1700000000)
{
    std::list<CBlockIndex> chain;
    CBlockIndex* prev{nullptr};
    const int first_height = tip_height - static_cast<int>(count) + 1;
    for (size_t i = 0; i < count; ++i) {
        auto& entry = chain.emplace_back();
        entry.nHeight = first_height + static_cast<int>(i);
        entry.nTime = static_cast<uint32_t>(start_time + static_cast<int64_t>(i) * spacing);
        entry.nBits = bits;
        entry.pprev = prev;
        prev = &entry;
    }
    return chain;
}
} // namespace

/**
 * What this chain does with difficulty, which is not what it forked from.
 *
 * GetNextWorkRequired has two branches and neither is Dash's: at or below
 * lastPowBlock it returns powLimit outright, and above it the target comes from
 * LWMA3 over the last 36 blocks. The cases below read the rule rather than a
 * table of amounts from another chain's mainnet.
 */
BOOST_AUTO_TEST_CASE(proof_of_work_heights_return_the_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const uint32_t pow_limit = UintToArith256(params.powLimit).GetCompact();

    CBlockHeader header;
    header.nTime = 1700000000;

    // Whatever the chain looks like, every block up to lastPowBlock is minimum
    // difficulty: this chain mines its proof-of-work era at the limit and puts
    // its real difficulty in the proof-of-stake era.
    for (const int tip : {0, 1, 500, params.lastPowBlock - 1}) {
        auto chain = MakeChain(tip, 1, params.nPowTargetSpacing, 0x1b104be1U);
        BOOST_CHECK_EQUAL(GetNextWorkRequired(&chain.back(), &header, params), pow_limit);
    }
}

BOOST_AUTO_TEST_CASE(the_short_chain_branch_is_unreachable_through_the_entry_point)
{
    // LWMA3 returns posLimit outright while the height is under its window of
    // 36. No network can ask it that: the proof-of-work branch answers for
    // every height up to lastPowBlock, and the smallest lastPowBlock any
    // network sets is 999. The branch is dead code, and this says so rather
    // than pretending to exercise it -- reaching it would need a chain of 36
    // entries that the entry point never routes there, which is how the suite
    // this replaces walked off the front of a fabricated chain and aborted.
    for (const std::string& net : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto chainParams = CreateChainParams(*m_node.args, net);
        BOOST_CHECK(chainParams->GetConsensus().lastPowBlock > 36);
    }
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK(CreateChainParams(*m_node.args, CBaseChainParams::DEVNET)->GetConsensus().lastPowBlock > 36);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_CASE(lwma_holds_the_target_when_blocks_arrive_on_time)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const int64_t T = params.posTargetSpacing;
    const uint32_t bits = 0x1b104be1U;

    CBlockHeader header;
    header.nTime = 1700000000;

    // Every block exactly on target: the weighted solvetime sum is exactly k,
    // so the next target is the one the chain already has. This is the
    // property the whole algorithm is built around, and it holds without any
    // number from another chain appearing in the test.
    auto chain = MakeChain(200000, 60, T, bits);
    const uint32_t steady = GetNextWorkRequired(&chain.back(), &header, params);
    arith_uint256 held, produced;
    held.SetCompact(bits);
    produced.SetCompact(steady);
    // Compact form keeps 24 bits of mantissa, and the average truncates, so
    // the two agree to within a part in a million rather than exactly.
    BOOST_CHECK(produced <= held);
    BOOST_CHECK(produced > held - held / 1000000);
}

BOOST_AUTO_TEST_CASE(lwma_moves_the_target_with_the_block_rate)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const int64_t T = params.posTargetSpacing;
    const uint32_t bits = 0x1b104be1U;

    CBlockHeader header;
    header.nTime = 1700000000;

    auto on_time = MakeChain(200000, 60, T, bits);
    auto fast = MakeChain(200000, 60, T / 2, bits);
    auto slow = MakeChain(200000, 60, T * 2, bits);

    arith_uint256 steady, quicker, slower;
    steady.SetCompact(GetNextWorkRequired(&on_time.back(), &header, params));
    quicker.SetCompact(GetNextWorkRequired(&fast.back(), &header, params));
    slower.SetCompact(GetNextWorkRequired(&slow.back(), &header, params));

    // Blocks arriving twice as fast make the target smaller, which is harder;
    // half as fast makes it larger. A difficulty rule that got this backwards
    // would run away in one direction, and nothing else here would notice.
    BOOST_CHECK(quicker < steady);
    BOOST_CHECK(slower > steady);
}

BOOST_AUTO_TEST_CASE(lwma_never_answers_past_the_stake_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& params = chainParams->GetConsensus();
    const arith_uint256 pos_limit = UintToArith256(params.posLimit);

    CBlockHeader header;
    header.nTime = 1700000000;

    // A chain that stalled: every solvetime is clamped at six times the target
    // spacing, and the answer is clamped at the limit rather than running past
    // it. Started from a target already close to the limit, so the clamp is
    // what decides the result.
    auto stalled = MakeChain(200000, 60, params.posTargetSpacing * 100, pos_limit.GetCompact());
    arith_uint256 answer;
    answer.SetCompact(GetNextWorkRequired(&stalled.back(), &header, params));
    BOOST_CHECK(answer <= pos_limit);
}

// BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
// {
//     const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

//     int64_t nLastRetargetTime = 1231006505; // Block #0
//     CBlockIndex pindexLast;
//     pindexLast.nHeight = 2015;
//     pindexLast.nTime = 1233061996;  // Block #2015
//     pindexLast.nBits = 0x1d00ffff;
//     BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00ffffU);
// }

/* Test the constraint on the lower bound for actual time taken */
// BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
// {
//     const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

//     int64_t nLastRetargetTime = 1279008237; // Block #66528
//     CBlockIndex pindexLast;
//     pindexLast.nHeight = 68543;
//     pindexLast.nTime = 1279297671;  // Block #68543
//     pindexLast.nBits = 0x1c05a3f4;
//     BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1c0168fdU);
// }

/* Test the constraint on the upper bound for actual time taken */
// BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
// {
//     const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

//     int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
//     CBlockIndex pindexLast;
//     pindexLast.nHeight = 46367;
//     pindexLast.nTime = 1269211443;  // Block #46367
//     pindexLast.nBits = 0x1c387f6f;
//     BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00e1fdU);
// }

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits = ~0x00800000;
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

BOOST_AUTO_TEST_SUITE_END()
