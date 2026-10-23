// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <evo/cbtx.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <llmq/chainlocks.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/options.h>
#include <llmq/quorums.h>
#include <llmq/utils.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <streams.h>
#include <util/check.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

struct QuorumHistoryReadSetup : RegTestingSetup {
    QuorumHistoryReadSetup() : RegTestingSetup({"-testactivationheight=v20@1"}) {}
};

BOOST_FIXTURE_TEST_SUITE(evo_cbtx_tests, TestChain100Setup)

static CBlock BlockWithCbTx(const CCbTx& cbtx)
{
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_COINBASE;
    tx.vin.emplace_back();
    tx.vout.emplace_back(0, CScript() << OP_RETURN);
    SetTxPayload(tx, cbtx);

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(tx));
    return block;
}

// An attacker-supplied bestCLHeightDiff used to be subtracted from the block
// height without a range check; a diff at or above the height yields a
// negative chainlock height, GetAncestor() returns nullptr for it, and the
// unchecked dereference crashed the node on a crafted block. (dash#7363)
BOOST_AUTO_TEST_CASE(bestclheightdiff_out_of_range_is_rejected)
{
    const auto& clhandler = *Assert(m_node.llmq_ctx)->clhandler;

    // A fake index detached from any chain: pprev == nullptr means the
    // previous coinbase carries no chainlock, which is the path that reaches
    // the range check.
    CBlockIndex index;
    index.nHeight = 5;

    CBLSSecretKey sk;
    sk.MakeNewKey();

    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.nHeight = index.nHeight;
    cbtx.bestCLSignature = sk.Sign(uint256::ONE, false);
    BOOST_REQUIRE(cbtx.bestCLSignature.IsValid());

    // diff == nHeight points one block before genesis
    cbtx.bestCLHeightDiff = index.nHeight;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");
    }

    // The crafted-block extreme: bestCLHeightDiff travels as a CompactSize, so
    // MAX_SIZE is the largest value that deserializes at all
    cbtx.bestCLHeightDiff = MAX_SIZE;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");
    }
}

BOOST_AUTO_TEST_CASE(null_clsig_rules_unchanged)
{
    const auto& clhandler = *Assert(m_node.llmq_ctx)->clhandler;

    CBlockIndex index;
    index.nHeight = 5;

    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.nHeight = index.nHeight;

    // A null signature with a zero diff is the legitimate "no chainlock yet" form
    {
        BlockValidationState state;
        BOOST_CHECK(CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
    }

    // A null signature with a non-zero diff stays rejected
    cbtx.bestCLHeightDiff = 1;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(cbtx), &index, clhandler, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");
    }
}

BOOST_AUTO_TEST_CASE(historical_chainlock_read_failure_is_local_error)
{
    const auto& clhandler = *Assert(m_node.llmq_ctx)->clhandler;
    LOCK(cs_main);
    CBlock previous;
    BOOST_REQUIRE(ReadBlockFromDisk(previous, m_node.chainman->ActiveChain().Tip(), Params().GetConsensus()));
    const uint256 previous_hash = previous.GetHash();
    CBlockIndex previous_index;
    previous_index.phashBlock = &previous_hash;
    previous_index.nHeight = Params().GetConsensus().DIP0003Height;
    previous_index.nFile = 999;
    previous_index.nDataPos = 0;
    previous_index.nStatus = BLOCK_HAVE_DATA;

    CBLSSecretKey sk;
    sk.MakeNewKey();
    CCbTx previous_cbtx;
    previous_cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    previous_cbtx.bestCLSignature = sk.Sign(uint256::ONE, false);
    previous_cbtx.bestCLHeightDiff = 3;
    previous.vtx[0] = BlockWithCbTx(previous_cbtx).vtx[0];
    const auto write_previous = [&] {
        // A helper-level fixture: retain a mined header, vary only its historical payload.
        // This is not a claim that the synthetic block passes full block validation.
        CAutoFile file(OpenBlockFile(FlatFilePos{999, 0}), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!file.IsNull());
        file << previous;
    };
    write_previous();
    BOOST_REQUIRE(GetNonNullCoinbaseChainlock(&previous_index).has_value());

    CBlockIndex current_index;
    current_index.nHeight = previous_index.nHeight + 1;
    current_index.pprev = &previous_index;
    CCbTx current_cbtx;
    current_cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    current_cbtx.nHeight = current_index.nHeight;
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(current_cbtx), &current_index, clhandler, state));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-null-clsig");
    }

    // Inject an unreadable historical position, without altering a real fixture blockfile.
    previous_index.nFile = 998;
    BOOST_CHECK_THROW(GetNonNullCoinbaseChainlock(&previous_index), CoinbaseChainlockReadError);
    {
        BlockValidationState state;
        BOOST_CHECK(!CheckCbTxBestChainlock(BlockWithCbTx(current_cbtx), &current_index, clhandler, state));
        BOOST_CHECK(state.IsError());
        BOOST_CHECK(!state.IsInvalid());
    }
    uint32_t diff{42};
    CBLSSignature sig;
    BOOST_CHECK_THROW(CalcCbTxBestChainlock(clhandler, &previous_index, diff, sig), CoinbaseChainlockReadError);

    // The same null candidate remains valid when historical absence is genuine.
    previous_index.nFile = 999;
    previous_cbtx.bestCLSignature.Reset();
    previous.vtx[0] = BlockWithCbTx(previous_cbtx).vtx[0];
    write_previous();
    {
        BlockValidationState state;
        BOOST_CHECK(CheckCbTxBestChainlock(BlockWithCbTx(current_cbtx), &current_index, clhandler, state));
    }
    previous_cbtx.nVersion = CCbTx::Version::MERKLE_ROOT_QUORUMS;
    previous.vtx[0] = BlockWithCbTx(previous_cbtx).vtx[0];
    write_previous();
    BOOST_CHECK(!GetNonNullCoinbaseChainlock(&previous_index).has_value());
    previous_index.nHeight = Params().GetConsensus().DIP0003Height - 1;
    previous_index.nFile = 998;
    BOOST_CHECK(!GetNonNullCoinbaseChainlock(&previous_index).has_value());
    BOOST_CHECK(!GetNonNullCoinbaseChainlock(nullptr).has_value());
}

BOOST_AUTO_TEST_CASE(malformed_historical_chainlock_is_not_absence)
{
    LOCK(cs_main);
    CBlock previous;
    BOOST_REQUIRE(ReadBlockFromDisk(previous, m_node.chainman->ActiveChain().Tip(), Params().GetConsensus()));
    const uint256 previous_hash = previous.GetHash();
    CBlockIndex index;
    index.phashBlock = &previous_hash;
    index.nHeight = Params().GetConsensus().DIP0003Height;
    index.nFile = 999;
    index.nStatus = BLOCK_HAVE_DATA;
    // At DIP3 this ordinary historical coinbase cannot be interpreted as an absent v3 signature.
    {
        CAutoFile file(OpenBlockFile(FlatFilePos{999, 0}), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!file.IsNull());
        file << previous;
    }
    BOOST_CHECK_THROW(GetNonNullCoinbaseChainlock(&index), CoinbaseChainlockReadError);
    CMutableTransaction malformed(*previous.vtx[0]);
    malformed.nType = TRANSACTION_COINBASE;
    malformed.vExtraPayload = {0xff};
    previous.vtx[0] = MakeTransactionRef(malformed);
    {
        CAutoFile file(OpenBlockFile(FlatFilePos{999, 0}), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!file.IsNull());
        file << previous;
    }
    BOOST_CHECK_THROW(GetNonNullCoinbaseChainlock(&index), CoinbaseChainlockReadError);
}

BOOST_FIXTURE_TEST_CASE(quorum_history_read_failure_is_local_block_error, QuorumHistoryReadSetup)
{
    LOCK(cs_main);
    const auto& consensus = Params().GetConsensus();
    auto& ctx = *Assert(m_node.llmq_ctx);
    const auto params = *Assert(Params().GetLLMQ(consensus.llmqTypeChainLocks));
    // Synthetic indexed history isolates the membership read; this does not claim
    // that the synthetic chain or the candidate passes full block validation.
    CBlockIndex* previous = m_node.chainman->m_blockman.LookupBlockIndex(m_node.chainman->ActiveTip()->GetBlockHash());
    for (int height = 1; height <= 576; ++height) {
        CBlockIndex* index = m_node.chainman->m_blockman.InsertBlockIndex(GetRandHash());
        index->pprev = previous;
        index->nHeight = height;
        index->nStatus = BLOCK_HAVE_DATA;
        index->nFile = 998; // No such file in the disposable test datadir.
        index->BuildSkip();
        previous = index;
    }
    BOOST_REQUIRE_EQUAL(previous->nHeight % params.dkgInterval, 0);
    BOOST_REQUIRE(DeploymentActiveAfter(previous->GetAncestor(568), consensus, Consensus::DEPLOYMENT_V20));
    // Supply the unrelated credit-pool prerequisite through its persisted snapshot
    // format (height 576). The history error must arise inside the tx loop, not
    // while reconstructing the credit pool before that loop.
    m_node.evodb->Write(std::make_pair(std::string{"cpm_S"}, previous->GetBlockHash()), CCreditPool{});
    BOOST_REQUIRE_EQUAL(m_node.cpoolman->GetCreditPool(previous, consensus).locked, 0);

    llmq::CFinalCommitment qc(params, previous->GetBlockHash());
    qc.nVersion = llmq::CFinalCommitment::GetVersion(llmq::IsQuorumRotationEnabled(params, previous),
        DeploymentActiveAfter(previous, consensus, Consensus::DEPLOYMENT_V19));
    std::fill(qc.signers.begin(), qc.signers.end(), true);
    std::fill(qc.validMembers.begin(), qc.validMembers.end(), true);
    CBLSSecretKey key;
    key.MakeNewKey();
    qc.quorumPublicKey = key.GetPublicKey();
    qc.quorumVvecHash = GetRandHash();
    qc.membersSig = key.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
    qc.quorumSig = key.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
    // A failed lookup must not cache an empty membership or call the commitment invalid.
    BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(params.type, *m_node.dmnman, *ctx.qsnapman, previous), CoinbaseChainlockReadError);
    BOOST_CHECK_THROW(qc.Verify(*m_node.dmnman, *ctx.qsnapman, previous, false), CoinbaseChainlockReadError);

    llmq::CFinalCommitmentTxPayload payload;
    payload.nHeight = previous->nHeight + 1;
    payload.commitment = qc;
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_QUORUM_COMMITMENT;
    SetTxPayload(tx, payload);
    const auto& view = m_node.chainman->ActiveChainstate().CoinsTip();
    auto& processor = *m_node.chain_helper->special_tx;
    TxValidationState tx_state;
    BOOST_CHECK(!processor.CheckSpecialTx(CTransaction(tx), previous, view, false, tx_state));
    BOOST_REQUIRE(tx_state.IsError());
    BOOST_CHECK(!tx_state.IsInvalid());

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(tx));
    CBlockIndex index;
    index.nHeight = previous->nHeight + 1;
    index.pprev = previous;
    BlockValidationState state;
    std::optional<MNListUpdates> updates;
    BOOST_CHECK(!processor.ProcessSpecialTxsInBlock(block, &index, view, true, false, state, updates));
    BOOST_CHECK(state.IsError());
    BOOST_CHECK(!state.IsInvalid());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), tx_state.GetRejectReason());
}

BOOST_FIXTURE_TEST_CASE(mnhf_replay_preserves_local_chainlock_error, QuorumHistoryReadSetup)
{
    LOCK(cs_main);
    const auto& consensus = Params().GetConsensus();
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    auto& manager = *m_node.mnhf_manager;
    CBlock history;
    BOOST_REQUIRE(ReadBlockFromDisk(history, m_node.chainman->ActiveTip(), consensus));
    CBlockIndex* previous = m_node.chainman->m_blockman.LookupBlockIndex(m_node.chainman->ActiveTip()->GetBlockHash());
    for (int height = 1; height <= 576; ++height) {
        CBlockIndex* index = m_node.chainman->m_blockman.InsertBlockIndex(GetRandHash());
        index->pprev = previous;
        index->nHeight = height;
        index->nStatus = BLOCK_HAVE_DATA;
        index->nFile = 998;
        index->BuildSkip();
        previous = index;
    }
    const auto params = *Assert(Params().GetLLMQ(consensus.llmqTypeMnhf));
    BOOST_REQUIRE_EQUAL(previous->nHeight % params.dkgInterval, 0);
    llmq::CFinalCommitment qc(params, previous->GetBlockHash());
    // Persisted prerequisites make GetQuorum reconstruct this quorum on a cache miss.
    // The error must come from its unavailable work block, before signature verification.
    m_node.evodb->Write(std::make_pair(std::string{"q_mc"}, std::make_pair(params.type, qc.quorumHash)),
                       std::make_pair(qc, previous->GetBlockHash()));
    m_node.evodb->Write(std::make_pair(std::string{"mnhf_s2"}, previous->GetBlockHash()), CMNHFManager::Signals{});
    BOOST_CHECK_THROW(qman.GetQuorum(params.type, qc.quorumHash), CoinbaseChainlockReadError);

    MNHFTxPayload payload;
    payload.signal.versionBit = 27;
    payload.signal.quorumHash = qc.quorumHash;
    BOOST_REQUIRE(Params().IsValidMNActivation(payload.signal.versionBit, previous->GetMedianTimePast()));
    history.vtx.resize(1);
    history.vtx.push_back(MakeTransactionRef(payload.PrepareTx()));
    // Retain a valid PoW header, choose a fresh hash to avoid real history/cache keys.
    // The modified body is only an MNHF replay fixture, not a fully valid block.
    do { ++history.nNonce; } while (!CheckProofOfWork(history.GetHash(), history.nBits, consensus));
    CBlockIndex* ancestor = m_node.chainman->m_blockman.InsertBlockIndex(history.GetHash());
    ancestor->pprev = previous;
    ancestor->nHeight = previous->nHeight + 1;
    ancestor->nFile = 999;
    ancestor->nDataPos = 0;
    ancestor->nStatus = BLOCK_HAVE_DATA;
    ancestor->BuildSkip();
    {
        CAutoFile file(OpenBlockFile(FlatFilePos{999, 0}), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!file.IsNull());
        file << history;
    }

    BlockValidationState direct;
    BOOST_CHECK(!manager.ProcessBlock(history, ancestor, true, direct).has_value());
    BOOST_REQUIRE(direct.IsError());
    BOOST_CHECK(!direct.IsInvalid());
    BOOST_CHECK_THROW(manager.GetSignalsStage(ancestor), CoinbaseChainlockReadError);
    // A current candidate with no MNHF tx still needs to replay its missing ancestor.
    CBlockIndex current;
    current.pprev = ancestor;
    current.nHeight = ancestor->nHeight + 1;
    BlockValidationState replay;
    BOOST_CHECK(!manager.ProcessBlock(CBlock{}, &current, true, replay).has_value());
    BOOST_CHECK(replay.IsError());
    BOOST_CHECK(!replay.IsInvalid());
    BOOST_CHECK_EQUAL(replay.GetRejectReason(), direct.GetRejectReason());
}

BOOST_AUTO_TEST_SUITE_END()
