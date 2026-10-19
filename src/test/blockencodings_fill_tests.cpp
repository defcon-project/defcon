// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compact block reconstruction must refuse a second use of a partial block
// instead of asserting (bitcoin#26898, bitcoin#33296 via dash#7398). These
// cases live outside blockencodings_tests, which CI excludes for unrelated
// failures of its own (see the unit step in .github/workflows/build.yml).

#include <blockencodings.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <validation.h>
#include <pow.h>
#include <streams.h>
#include <util/strencodings.h>
#include <txmempool.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(blockencodings_fill_tests, RegTestingSetup)

// A block of a coinbase and two other transactions, none of them in a mempool,
// so a compact block of it leaves both non-coinbase transactions missing.
static CBlock BuildThreeTxBlock()
{
    CBlock block;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig.resize(10);
    tx.vout.resize(1);
    tx.vout[0].nValue = 42;

    block.vtx.resize(3);
    block.vtx[0] = MakeTransactionRef(tx);
    block.nVersion = 42;
    block.hashPrevBlock = InsecureRand256();
    block.nBits = 0x207fffff;

    tx.vin[0].prevout.hash = InsecureRand256();
    tx.vin[0].prevout.n = 0;
    block.vtx[1] = MakeTransactionRef(tx);

    tx.vin.resize(10);
    for (size_t i = 0; i < tx.vin.size(); i++) {
        tx.vin[i].prevout.hash = InsecureRand256();
        tx.vin[i].prevout.n = 0;
    }
    block.vtx[2] = MakeTransactionRef(tx);

    bool mutated;
    block.hashMerkleRoot = BlockMerkleRoot(block, &mutated);
    assert(!mutated);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;
    return block;
}

// A reconstruction that fails on the merkle root leaves the partial block in
// flight, with its header already wiped, while the full block is fetched. A
// second answer to the same request must be refused, not hit an assert.
BOOST_AUTO_TEST_CASE(fill_block_after_failed_reconstruction)
{
    CTxMemPool pool;
    const std::vector<std::pair<uint256, CTransactionRef>> extra_txn;
    CBlock block(BuildThreeTxBlock());

    LOCK2(cs_main, pool.cs);
    PartiallyDownloadedBlock partialBlock(&pool);
    BOOST_CHECK(partialBlock.InitData(CBlockHeaderAndShortTxIDs{block}, extra_txn) == READ_STATUS_OK);

    // The two missing transactions in the wrong order.
    CBlock block2;
    BOOST_CHECK(partialBlock.FillBlock(block2, {block.vtx[2], block.vtx[1]}) == READ_STATUS_FAILED);
    BOOST_CHECK(partialBlock.header.IsNull());

    CBlock block3;
    BOOST_CHECK(partialBlock.FillBlock(block3, {block.vtx[1], block.vtx[2]}) == READ_STATUS_INVALID);
    BOOST_CHECK(!partialBlock.IsTxAvailable(0));
}

BOOST_AUTO_TEST_CASE(init_data_twice)
{
    CTxMemPool pool;
    const std::vector<std::pair<uint256, CTransactionRef>> extra_txn;
    CBlock block(BuildThreeTxBlock());

    LOCK2(cs_main, pool.cs);
    PartiallyDownloadedBlock partialBlock(&pool);
    BOOST_CHECK(partialBlock.InitData(CBlockHeaderAndShortTxIDs{block}, extra_txn) == READ_STATUS_OK);
    BOOST_CHECK(partialBlock.InitData(CBlockHeaderAndShortTxIDs{block}, extra_txn) == READ_STATUS_INVALID);
}


static CBlock BuildThreeTxStakeBlock()
{
    CBlock block{BuildThreeTxBlock()};
    CMutableTransaction stake{*block.vtx[1]};
    stake.vout[0].SetEmpty();
    stake.vout.emplace_back(42, CScript{});
    block.vtx[1] = MakeTransactionRef(stake);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    assert(block.IsProofOfStake());
    return block;
}

// Export actual production-serialized compact blocks for the Python wire-format regression.
// The signature vector is carried even for a synthetic PoW fixture; its bytes must survive
// each Python representation just as they do for a signed PoS block.
BOOST_AUTO_TEST_CASE(compact_block_wire_fixtures)
{
    for (size_t fixture = 0; fixture < 3; ++fixture) {
        const std::vector<unsigned char> signature = fixture == 0 ? std::vector<unsigned char>{} : std::vector<unsigned char>{0x12, 0x34, 0x56};
        CBlock block{fixture == 2 ? BuildThreeTxStakeBlock() : BuildThreeTxBlock()};
        block.vchBlockSig = signature;
        CDataStream full{SER_NETWORK, PROTOCOL_VERSION};
        full << block;
        CDataStream compact{SER_NETWORK, PROTOCOL_VERSION};
        compact << CBlockHeaderAndShortTxIDs{block};
        CDataStream reader{compact};
        CBlockHeaderAndShortTxIDs decoded;
        reader >> decoded;
        BOOST_CHECK(decoded.vchBlockSig == signature);
        BOOST_CHECK_EQUAL(decoded.BlockTxCount(), block.vtx.size());
        BOOST_CHECK(reader.empty());
        BOOST_TEST_MESSAGE("COMPACT_WIRE_FIXTURE " << HexStr(full) << " " << HexStr(compact));
    }
}


// Compact reconstruction checks the body against the header, then the unchanged full validator
// checks all consensus rules. A reconstruction alone must not mark those full checks complete.
BOOST_AUTO_TEST_CASE(reconstruction_defers_full_block_validation)
{
    CTxMemPool pool;
    const CBlock block{BuildThreeTxBlock()};
    PartiallyDownloadedBlock partial{&pool};
    BOOST_REQUIRE(partial.InitData(CBlockHeaderAndShortTxIDs{block}, {}) == READ_STATUS_OK);
    CBlock reconstructed;
    BOOST_REQUIRE(partial.FillBlock(reconstructed, {block.vtx[1], block.vtx[2]}) == READ_STATUS_OK);
    BOOST_CHECK(!reconstructed.fChecked);
    BlockValidationState state;
    BOOST_REQUIRE(CheckBlock(reconstructed, state, Params().GetConsensus()));
    BOOST_CHECK(reconstructed.fChecked);
}

BOOST_AUTO_TEST_CASE(reconstructed_stake_still_requires_block_signature)
{
    CTxMemPool pool;
    const CBlock block{BuildThreeTxStakeBlock()};
    PartiallyDownloadedBlock partial{&pool};
    BOOST_REQUIRE(partial.InitData(CBlockHeaderAndShortTxIDs{block}, {}) == READ_STATUS_OK);
    CBlock reconstructed;
    BOOST_CHECK(partial.FillBlock(reconstructed, {block.vtx[1], block.vtx[2]}) == READ_STATUS_OK);
    BOOST_CHECK(reconstructed.IsProofOfStake());
    BOOST_CHECK(!reconstructed.fChecked);
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(reconstructed, state, Params().GetConsensus()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-sign");
}

BOOST_AUTO_TEST_SUITE_END()
