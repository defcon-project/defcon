// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <key.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <serialize.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <vector>

#include <boost/test/unit_test.hpp>

// Which CheckBlock failures may mark a block's header invalid. The header hash
// commits to the header, the merkle root to the transactions; a proof-of-stake
// block's signature is committed to by neither, so a failure that depends on it
// (the signature itself, or the size it adds) must not mark the header invalid:
// it is BLOCK_MUTATED. So is a body that disagrees with the header about being
// proof of stake and is not the body the header commits to.

namespace {
struct BlockCheckSetup : public BasicTestingSetup {
    BlockCheckSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};

constexpr uint32_t EASY_BITS{0x207fffff};
constexpr uint32_t HARD_BITS{0x1d00ffff};

CMutableTransaction Coinbase(bool proof_of_stake)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_1 << OP_0;
    coinbase.vout.resize(1);
    if (proof_of_stake) {
        coinbase.vout[0].SetEmpty();
    } else {
        coinbase.vout[0] = CTxOut(500 * COIN, CScript() << OP_TRUE);
    }
    return coinbase;
}

CMutableTransaction Filler(uint32_t n, size_t bytes)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S("03"), n);
    tx.vout.resize(1);
    tx.vout[0] = CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>(bytes, 0));
    return tx;
}

//! A proof-of-stake block as CheckBlock reads one: an empty coinbase output, a
//! coinstake in vtx[1] paying a pay-to-pubkey output to the staker, nNonce 0,
//! and the staker's signature over the header hash.
CBlock MakePosBlock(const CKey& key, uint32_t bits = EASY_BITS, const std::vector<CMutableTransaction>& extra = {})
{
    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = uint256S("01");
    block.nTime = 1700000000;
    block.nBits = bits;
    block.nNonce = 0;

    CMutableTransaction coinstake;
    coinstake.vin.resize(1);
    coinstake.vin[0].prevout = COutPoint(uint256S("02"), 0);
    coinstake.vout.resize(2);
    coinstake.vout[0].SetEmpty();
    coinstake.vout[1] = CTxOut(500 * COIN, GetScriptForRawPubKey(key.GetPubKey()));

    block.vtx.push_back(MakeTransactionRef(Coinbase(/*proof_of_stake=*/true)));
    block.vtx.push_back(MakeTransactionRef(coinstake));
    for (const auto& tx : extra) block.vtx.push_back(MakeTransactionRef(tx));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    BOOST_REQUIRE(block.IsProofOfStake());
    BOOST_REQUIRE(key.Sign(block.GetHash(), block.vchBlockSig));
    return block;
}

//! A proof-of-work block whose nNonce is 0, which the header alone reads as
//! proof of stake.
CBlock MakeZeroNoncePowBlock(uint32_t bits)
{
    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = uint256S("01");
    block.nTime = 1700000000;
    block.nBits = bits;
    block.nNonce = 0;
    block.vtx.push_back(MakeTransactionRef(Coinbase(/*proof_of_stake=*/false)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

bool HeaderSaysProofOfStake(const CBlock& block) { return static_cast<const CBlockHeader&>(block).IsProofOfStake(); }

CKey NewKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(block_import_check_tests, BlockCheckSetup)

BOOST_AUTO_TEST_CASE(a_valid_proof_of_stake_block_passes)
{
    const CKey key{NewKey()};
    CBlock block{MakePosBlock(key)};
    BlockValidationState state;
    BOOST_CHECK(CheckBlock(block, state, Params().GetConsensus()));
    BOOST_CHECK_EQUAL(GetCommittedBlockSize(block),
                      ::GetSerializeSize(block, PROTOCOL_VERSION) - ::GetSerializeSize(block.vchBlockSig, PROTOCOL_VERSION) + 1);
}

BOOST_AUTO_TEST_CASE(a_bad_signature_is_mutated)
{
    const CKey key{NewKey()};
    CBlock block{MakePosBlock(key)};
    const uint256 hash{block.GetHash()};
    block.vchBlockSig[block.vchBlockSig.size() / 2] ^= 0x01;
    BOOST_CHECK_EQUAL(block.GetHash(), hash);
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, Params().GetConsensus()));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-sign");
}

BOOST_AUTO_TEST_CASE(a_lengthened_signature_still_verifies)
{
    // Why the signature cannot count as committed: the lax DER parser accepts
    // bytes appended to a valid signature, and the hash does not change.
    const CKey key{NewKey()};
    CBlock block{MakePosBlock(key)};
    const uint256 hash{block.GetHash()};
    block.vchBlockSig.insert(block.vchBlockSig.end(), 16, 0);
    BOOST_CHECK_EQUAL(block.GetHash(), hash);
    BlockValidationState state;
    BOOST_CHECK(CheckBlock(block, state, Params().GetConsensus()));
}

BOOST_AUTO_TEST_CASE(a_signature_past_the_size_limit_is_mutated)
{
    const CKey key{NewKey()};
    CBlock block{MakePosBlock(key)};
    const size_t committed{GetCommittedBlockSize(block)};
    block.vchBlockSig.resize(block.vchBlockSig.size() + MaxBlockSize(), 0);
    BOOST_CHECK_GT(::GetSerializeSize(block, PROTOCOL_VERSION), MaxBlockSize());
    BOOST_CHECK_EQUAL(GetCommittedBlockSize(block), committed);
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, Params().GetConsensus()));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
}

BOOST_AUTO_TEST_CASE(a_committed_part_past_the_size_limit_is_consensus)
{
    const CKey key{NewKey()};
    CBlock block{MakePosBlock(key, EASY_BITS, {Filler(0, MaxBlockSize() / 2), Filler(1, MaxBlockSize() / 2)})};
    BOOST_CHECK_GT(GetCommittedBlockSize(block), MaxBlockSize());
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, Params().GetConsensus()));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CONSENSUS);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
}

BOOST_AUTO_TEST_CASE(both_size_gates_tell_the_signature_from_the_committed_part)
{
    // CheckBlock uses the DIP0001 limit, ContextualCheckBlock the limit for the
    // block's height; a block over the larger one never reaches the smaller
    // one, so the shared check is held to both limits here.
    const unsigned int small_limit{MaxBlockSize(/*fDIP0001Active=*/false)};
    const unsigned int large_limit{MaxBlockSize()};
    BOOST_REQUIRE_LT(small_limit, large_limit);
    const CKey key{NewKey()};

    CBlock long_signature{MakePosBlock(key)};
    long_signature.vchBlockSig.resize(long_signature.vchBlockSig.size() + small_limit, 0);
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckBlockSize(long_signature, small_limit, state));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
    }
    {
        BlockValidationState state;
        BOOST_CHECK(CheckBlockSize(long_signature, large_limit, state));
    }

    const CBlock large_body{MakePosBlock(key, EASY_BITS, {Filler(0, small_limit)})};
    BOOST_CHECK_GT(GetCommittedBlockSize(large_body), small_limit);
    BOOST_CHECK_LT(GetCommittedBlockSize(large_body), large_limit);
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckBlockSize(large_body, small_limit, state));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CONSENSUS);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
    }
    {
        BlockValidationState state;
        BOOST_CHECK(CheckBlockSize(large_body, large_limit, state));
    }
}

BOOST_AUTO_TEST_CASE(a_damaged_coinstake_is_mutated)
{
    // A coinstake that is no longer one makes the body read as proof of work,
    // while the hash, through nNonce 0, still says proof of stake. Against a
    // target the hash misses, checking the proof of work first would fail the
    // header itself ("high-hash", BLOCK_INVALID_HEADER).
    const auto& params{Params().GetConsensus()};
    const CKey key{NewKey()};
    CBlock block{MakePosBlock(key, HARD_BITS)};
    const uint256 hash{block.GetHash()};
    BOOST_REQUIRE(!CheckProofOfWork(hash, block.nBits, params));
    CMutableTransaction damaged{*block.vtx[1]};
    damaged.vout[0] = CTxOut(1, CScript() << OP_TRUE);
    block.vtx[1] = MakeTransactionRef(damaged);
    BOOST_REQUIRE(!block.IsProofOfStake());
    BOOST_REQUIRE(HeaderSaysProofOfStake(block));
    BOOST_CHECK_EQUAL(block.GetHash(), hash);
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, params));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txnmrklroot");
}

BOOST_AUTO_TEST_CASE(a_zero_nonce_proof_of_work_block_is_judged_as_before)
{
    const auto& params{Params().GetConsensus()};

    // The body is the committed one, so the earlier merkle check passes and the
    // proof of work decides, as before: valid against an easy target ...
    CBlock easy{MakeZeroNoncePowBlock(EASY_BITS)};
    while (!CheckProofOfWork(easy.GetHash(), easy.nBits, params)) {
        ++easy.nTime;
    }
    BOOST_REQUIRE(HeaderSaysProofOfStake(easy));
    BOOST_REQUIRE(!easy.IsProofOfStake());
    {
        BlockValidationState state;
        BOOST_CHECK(CheckBlock(easy, state, params));
    }

    // ... and an invalid header against a hard one.
    CBlock hard{MakeZeroNoncePowBlock(HARD_BITS)};
    BOOST_REQUIRE(!CheckProofOfWork(hard.GetHash(), hard.nBits, params));
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckBlock(hard, state, params));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_INVALID_HEADER);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "high-hash");
    }
}

BOOST_AUTO_TEST_SUITE_END()
