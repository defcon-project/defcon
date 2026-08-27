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

BOOST_AUTO_TEST_SUITE_END()
