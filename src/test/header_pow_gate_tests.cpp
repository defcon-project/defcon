// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <pow.h>
#include <primitives/block.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(header_pow_gate_tests, TestChain100Setup)

/**
 * A header in the proof-of-work era must carry proof of work.
 *
 * AcceptBlockHeader used to decide whether to check the work from the header's
 * own nNonce (nNonce == 0 marks a header proof-of-stake), which whoever sent
 * the header chooses. So a header labelled proof-of-stake skipped the check and
 * could extend the header chain for free. The requirement is a function of
 * height instead -- the same lastPowBlock boundary the block-level rule uses --
 * and the previous block fixes the height.
 *
 * This crafts a header at a proof-of-work height with a correct target and a
 * valid time, but a hash above the target -- no valid work -- and nNonce == 0.
 * Under the old rule it was accepted; it must now be rejected.
 */
BOOST_AUTO_TEST_CASE(pow_era_header_without_work_is_rejected)
{
    ChainstateManager& chainman = *m_node.chainman;
    const CChainParams& chainparams = Params();
    const Consensus::Params& consensus = chainparams.GetConsensus();

    CBlockIndex* tip = nullptr;
    {
        LOCK(cs_main);
        tip = chainman.ActiveChain().Tip();
    }
    BOOST_REQUIRE(tip != nullptr);
    // The 100 mined regtest blocks and the next height are all in the PoW era.
    BOOST_REQUIRE_LE(tip->nHeight + 1, consensus.lastPowBlock);

    CBlockHeader header;
    header.nVersion = tip->nVersion;
    header.hashPrevBlock = tip->GetBlockHash();
    header.nTime = tip->GetMedianTimePast() + 1;
    header.nBits = GetNextWorkRequired(tip, &header, consensus);
    header.nNonce = 0;

    // hashMerkleRoot is not constrained by header validation, so grind it until
    // the header hash is above the target -- a header with no valid work.
    uint32_t salt = 1;
    for (; salt < 100000; ++salt) {
        header.hashMerkleRoot = ArithToUint256(arith_uint256(salt));
        if (!CheckProofOfWork(header.GetHash(), header.nBits, consensus)) break;
    }
    BOOST_REQUIRE(!CheckProofOfWork(header.GetHash(), header.nBits, consensus));

    BlockValidationState state;
    const bool accepted = chainman.ProcessNewBlockHeaders({header}, state, chainparams);
    BOOST_CHECK(!accepted);
    BOOST_CHECK(!state.IsValid());
}

BOOST_AUTO_TEST_SUITE_END()
