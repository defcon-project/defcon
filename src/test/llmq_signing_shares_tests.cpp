// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/common.h>
#include <llmq/signing_shares.h>
#include <streams.h>
#include <uint256.h>
#include <version.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;

namespace {

uint256 GetTestQuorumHash(uint32_t n)
{
    uint256 h;
    WriteLE32(h.begin(), n);
    return h;
}

CSigSesAnn MakeSigSesAnn(uint32_t session_id, uint32_t nonce, Consensus::LLMQType llmq_type = Consensus::LLMQType::LLMQ_50_60)
{
    return CSigSesAnn{session_id, llmq_type, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(nonce)};
}

CSigShare MakeSigShare(uint32_t nonce, Consensus::LLMQType llmq_type = Consensus::LLMQType::LLMQ_50_60)
{
    CSigShare sig_share{llmq_type, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(nonce), 1, CBLSLazySignature{}};
    sig_share.UpdateKey();
    return sig_share;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_signing_shares_tests, BasicTestingSetup)

// dash#7351, adapted: announcements are bounded per peer, but a refresh of an
// already-announced session must stay allowed, or an honest peer re-announcing
// with a new session id would wedge at the cap.
BOOST_AUTO_TEST_CASE(sig_ses_ann_respects_session_limit_but_allows_refresh)
{
    CSigSharesNodeState node_state;

    const CSigSesAnn ann1{MakeSigSesAnn(1, 1)};
    const CSigSesAnn ann2{MakeSigSesAnn(2, 2)};
    const CSigSesAnn ann3{MakeSigSesAnn(3, 3)};
    constexpr size_t max_sessions{2};

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 1U);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann2, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann2);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), max_sessions);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), max_sessions);

    BOOST_CHECK(!node_state.CanCreateSessionFromAnn(ann3, max_sessions));

    const CSigSesAnn ann1_refresh{4, Consensus::LLMQType::LLMQ_50_60, ann1.getQuorumHash(), ann1.getId(), ann1.getMsgHash()};
    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1_refresh, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1_refresh);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), max_sessions);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), max_sessions);
}

// A session the peer only knows because we sent it shares carries no announcement,
// so it must not consume the peer's announcement budget.
BOOST_AUTO_TEST_CASE(sig_ses_ann_limit_ignores_send_only_sessions)
{
    CSigSharesNodeState node_state;

    constexpr size_t max_sessions{1};
    const CSigShare sig_share{MakeSigShare(1)};
    const CSigSesAnn ann{MakeSigSesAnn(1, 2)};

    node_state.GetOrCreateSessionFromShare(sig_share);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), 0U);

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 2U);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);
}

BOOST_AUTO_TEST_CASE(sig_ses_ann_limit_is_per_llmq_type)
{
    CSigSharesNodeState node_state;

    constexpr size_t max_sessions{1};
    const CSigSesAnn ann1{MakeSigSesAnn(1, 1)};
    const CSigSesAnn ann2{MakeSigSesAnn(2, 2)};
    const CSigSesAnn other_type_ann{MakeSigSesAnn(3, 3, Consensus::LLMQType::LLMQ_400_60)};

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 1U);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);

    BOOST_CHECK(!node_state.CanCreateSessionFromAnn(ann2, max_sessions));
    BOOST_CHECK(node_state.CanCreateSessionFromAnn(other_type_ann, max_sessions));
    node_state.GetOrCreateSessionFromAnn(other_type_ann);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 2U);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_400_60), 1U);
}

// dash#7415, adapted: the intake caps consult SigShareMap::Size() on every share,
// so the running count must track every mutating method exactly.
BOOST_AUTO_TEST_CASE(sig_share_map_size_tracks_mutations)
{
    SigShareMap<CSigShare> sig_share_map;
    const CSigShare sig_share1{MakeSigShare(1)};
    const CSigShare sig_share2{MakeSigShare(2)};

    BOOST_CHECK(sig_share_map.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(!sig_share_map.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(sig_share_map.Add(sig_share2.GetKey(), sig_share2));
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 2U);

    sig_share_map.Erase(sig_share1.GetKey());
    sig_share_map.Erase(sig_share1.GetKey());
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 1U);

    sig_share_map.EraseAllForSignHash(sig_share2.GetSignHash());
    sig_share_map.EraseAllForSignHash(sig_share2.GetSignHash());
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 0U);
    BOOST_CHECK(sig_share_map.Empty());

    BOOST_CHECK(sig_share_map.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(sig_share_map.Add(sig_share2.GetKey(), sig_share2));
    sig_share_map.EraseIf([&](const SigShareKey& k, const CSigShare&) { return k == sig_share1.GetKey(); });
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 1U);

    sig_share_map.Clear();
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(sig_share_map_bucket_erase_updates_size)
{
    SigShareMap<CSigShare> sig_share_map;
    const auto sign_hash = MakeSigShare(1).GetSignHash();

    for (uint16_t member = 0; member < 5; ++member) {
        CSigShare s{Consensus::LLMQType::LLMQ_50_60, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(1),
                    member, CBLSLazySignature{}};
        s.UpdateKey();
        BOOST_CHECK_EQUAL(s.GetSignHash(), sign_hash);
        BOOST_CHECK(sig_share_map.Add(s.GetKey(), s));
    }
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 5U);

    sig_share_map.EraseAllForSignHash(sign_hash);
    BOOST_CHECK(sig_share_map.Empty());
}

BOOST_AUTO_TEST_CASE(pending_sig_shares_session_removal_updates_count)
{
    CSigSharesNodeState node_state;
    const CSigShare sig_share1{MakeSigShare(1)};
    const CSigShare sig_share2{MakeSigShare(2)};

    BOOST_CHECK(node_state.pendingIncomingSigShares.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(node_state.pendingIncomingSigShares.Add(sig_share2.GetKey(), sig_share2));
    BOOST_CHECK_EQUAL(node_state.pendingIncomingSigShares.Size(), 2U);

    node_state.RemoveSession(sig_share1.GetSignHash());
    BOOST_CHECK_EQUAL(node_state.pendingIncomingSigShares.Size(), 1U);
    BOOST_CHECK(!node_state.pendingIncomingSigShares.Has(sig_share1.GetKey()));
    BOOST_CHECK(node_state.pendingIncomingSigShares.Has(sig_share2.GetKey()));

    // Removing the same session twice, or a session with no pending shares, is a no-op.
    node_state.RemoveSession(sig_share1.GetSignHash());
    node_state.RemoveSession(MakeSigShare(3).GetSignHash());
    BOOST_CHECK_EQUAL(node_state.pendingIncomingSigShares.Size(), 1U);
}

namespace {

CBatchedSigShares MakeBatch(uint32_t session_id, size_t share_count)
{
    CBatchedSigShares batch;
    batch.sessionId = session_id;
    batch.sigShares.resize(share_count); // default pairs are enough to exercise the count invariant
    return batch;
}

} // namespace

// dash#7418, adapted: exercise the production QBSIGSHARES decoder directly. The
// outer batch count is bounded regardless of the inner contents; every batch here
// is empty, so only the outer-count guard can reject the stream, which keeps this
// a genuine test of that guard rather than an end-of-stream artifact.
BOOST_AUTO_TEST_CASE(qbsigshares_rejects_oversized_batch_count)
{
    std::vector<CBatchedSigShares> msgs(CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS + 1);

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    BOOST_CHECK_THROW(CSigSharesManager::UnserializeBatchedSigShares(stream), std::ios_base::failure);
}

// The regression this targets: each batch is individually within the cap, but the
// running aggregate exceeds MAX_MSGS_TOTAL_BATCHED_SIGS, so the decoder must abort
// mid-stream rather than accept the cross product of the per-batch limits.
BOOST_AUTO_TEST_CASE(qbsigshares_rejects_oversized_aggregate_total)
{
    std::vector<CBatchedSigShares> msgs;
    msgs.push_back(MakeBatch(1, 300));
    msgs.push_back(MakeBatch(2, 200)); // 300 + 200 = 500 > 400, though each batch is <= the cap

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    BOOST_CHECK_THROW(CSigSharesManager::UnserializeBatchedSigShares(stream), std::ios_base::failure);
}

// A single batch over the cap must be rejected by the running total as well.
BOOST_AUTO_TEST_CASE(qbsigshares_rejects_oversized_single_batch)
{
    std::vector<CBatchedSigShares> msgs;
    msgs.push_back(MakeBatch(1, CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS + 1));

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    BOOST_CHECK_THROW(CSigSharesManager::UnserializeBatchedSigShares(stream), std::ios_base::failure);
}

// Batches whose aggregate is exactly at the cap must be accepted and decoded intact.
BOOST_AUTO_TEST_CASE(qbsigshares_accepts_aggregate_at_cap)
{
    std::vector<CBatchedSigShares> msgs;
    msgs.push_back(MakeBatch(1, 200));
    msgs.push_back(MakeBatch(2, CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS - 200));

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    std::vector<CBatchedSigShares> decoded;
    BOOST_CHECK_NO_THROW(decoded = CSigSharesManager::UnserializeBatchedSigShares(stream));
    BOOST_CHECK_EQUAL(decoded.size(), 2U);
    BOOST_CHECK_EQUAL(decoded[0].sigShares.size(), 200U);
    BOOST_CHECK_EQUAL(decoded[1].sigShares.size(), CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS - 200);
}

BOOST_AUTO_TEST_SUITE_END()
