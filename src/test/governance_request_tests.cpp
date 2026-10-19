// Copyright (c) 2026 The Dash Core developers
// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/governance.h>
#include <governance/object.h>
#include <governance/vote.h>
#include <masternode/meta.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <protocol.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <util/time.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct GovernanceRequestSetup : TestingSetup {
    GovernanceRequestSetup() : TestingSetup{CBaseChainParams::MAIN}
    {
        // Request times are process-wide. Give each fixture a distinct vote hash and clock,
        // while individually executed before/after controls keep the same initial epoch.
        static int fixture_sequence{0};
        SetMockTime(1'700'000'000s + 1000s * fixture_sequence++);
        BOOST_REQUIRE(m_node.mn_metaman->LoadCache(false));
        BOOST_REQUIRE(m_node.govman->LoadCache(false));
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(false));
        m_node.mn_sync->SwitchToNextAsset();
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsSynced());
        static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).JumpOutOfIbd();
    }

    ~GovernanceRequestSetup()
    {
        static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).ResetIbd();
        SetMockTime(0s);
    }
};

size_t CountMessages(CNode& peer, const std::string& type)
{
    LOCK(peer.cs_vSend);
    return std::count_if(peer.vSendMsg.begin(), peer.vSendMsg.end(),
                         [&](const auto& msg) { return msg.m_type == type; });
}

size_t CountGetData(CNode& peer, const CInv& expected)
{
    LOCK(peer.cs_vSend);
    size_t count{0};
    for (const auto& msg : peer.vSendMsg) {
        if (msg.m_type != NetMsgType::GETDATA) continue;
        CDataStream stream{msg.data, SER_NETWORK, PROTOCOL_VERSION};
        std::vector<CInv> invs;
        stream >> invs;
        count += std::count_if(invs.begin(), invs.end(), [&](const CInv& inv) {
            return inv.type == expected.type && inv.hash == expected.hash;
        });
    }
    return count;
}

CGovernanceVote MakeVote()
{
    CGovernanceVote vote{COutPoint{uint256S("11"), 1}, uint256S("31"), VOTE_SIGNAL_FUNDING, VOTE_OUTCOME_YES};
    vote.SetTime(GetTime<std::chrono::seconds>().count());
    // A wire-valid size, but invalid unknown-parent authorship. In this fork ProcessVote
    // rejects it with 20 points before requesting its parent (dash#7527). That downstream
    // score is our observable authorization path; unsolicited/wrong-peer votes get none.
    vote.SetSignature(std::vector<unsigned char>(96, 0));
    return vote;
}

void SendVote(PeerManager& peerman, CNode& peer, const CGovernanceVote& vote)
    EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << vote;
    SendMessage(peerman, peer, NetMsgType::MNGOVERNANCEOBJECTVOTE, std::move(stream));
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(governance_request_tests, GovernanceRequestSetup)

BOOST_AUTO_TEST_CASE(vote_authorization_is_bound_to_announcing_peer)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto owner{MakeTestPeer(11)};
    auto other{MakeTestPeer(12)};
    m_node.peerman->InitializeNode(*owner, NODE_NETWORK);
    m_node.peerman->InitializeNode(*other, NODE_NETWORK);
    auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    const auto vote{MakeVote()};
    const CInv inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};
    AnnounceInv(*m_node.peerman, *owner, inv);
    connman.FlushSendBuffer(*other);
    SendVote(*m_node.peerman, *other, vote);
    BOOST_CHECK_EQUAL(CountMessages(*other, NetMsgType::MNGOVERNANCESYNC), 0U);
    // The wrong peer must not spend the owner's independent authorization.
    connman.FlushSendBuffer(*owner);
    SendVote(*m_node.peerman, *owner, vote);
    BOOST_CHECK_EQUAL(CountMessages(*owner, NetMsgType::MNGOVERNANCESYNC), 0U);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *owner), 20);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *other), 0);
    m_node.peerman->FinalizeNode(*owner);
    m_node.peerman->FinalizeNode(*other);
}

BOOST_AUTO_TEST_CASE(vote_authorization_is_bound_to_inventory_type)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto peer{MakeTestPeer(21)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    const auto vote{MakeVote()};
    AnnounceInv(*m_node.peerman, *peer, CInv{MSG_GOVERNANCE_OBJECT, vote.GetHash()});
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*peer);
    SendVote(*m_node.peerman, *peer, vote);
    BOOST_CHECK_EQUAL(CountMessages(*peer, NetMsgType::MNGOVERNANCESYNC), 0U);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 0);
    m_node.peerman->FinalizeNode(*peer);
}

BOOST_AUTO_TEST_CASE(unrequested_vote_preserves_other_peer_download)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto owner{MakeTestPeer(31)};
    auto other{MakeTestPeer(32)};
    m_node.peerman->InitializeNode(*owner, NODE_NETWORK);
    m_node.peerman->InitializeNode(*other, NODE_NETWORK);
    const auto vote{MakeVote()};
    const CInv inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};
    AnnounceInv(*m_node.peerman, *owner, inv);
    SendVote(*m_node.peerman, *other, vote);
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*owner);
    SetMockTime(GetTime<std::chrono::seconds>() + 61s);
    m_node.peerman->SendMessages(owner.get());
    BOOST_CHECK_EQUAL(CountGetData(*owner, inv), 1U);
    m_node.peerman->FinalizeNode(*owner);
    m_node.peerman->FinalizeNode(*other);
}

BOOST_AUTO_TEST_CASE(unsynced_vote_preserves_pending_request)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto peer{MakeTestPeer(41)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    const auto vote{MakeVote()};
    const CInv inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};
    AnnounceInv(*m_node.peerman, *peer, inv);
    m_node.mn_sync->Reset(true, false);
    BOOST_REQUIRE(!m_node.mn_sync->IsBlockchainSynced());
    SendVote(*m_node.peerman, *peer, vote);
    m_node.mn_sync->SwitchToNextAsset();
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*peer);
    SetMockTime(GetTime<std::chrono::seconds>() + 61s);
    m_node.peerman->SendMessages(peer.get());
    BOOST_CHECK_EQUAL(CountGetData(*peer, inv), 1U);
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*peer);
    SendVote(*m_node.peerman, *peer, vote);
    BOOST_CHECK_EQUAL(CountMessages(*peer, NetMsgType::MNGOVERNANCESYNC), 0U);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 20);
    m_node.peerman->FinalizeNode(*peer);
}

BOOST_AUTO_TEST_CASE(unsynced_object_preserves_pending_request)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto peer{MakeTestPeer(51)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    const CGovernanceObject object{uint256{}, 1, GetTime(), uint256S("51"), {}};
    const CInv inv{MSG_GOVERNANCE_OBJECT, object.GetHash()};
    AnnounceInv(*m_node.peerman, *peer, inv);
    m_node.mn_sync->Reset(true, false);
    BOOST_REQUIRE(!m_node.mn_sync->IsBlockchainSynced());
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << object;
    SendMessage(*m_node.peerman, *peer, NetMsgType::MNGOVERNANCEOBJECT, std::move(stream));
    m_node.mn_sync->SwitchToNextAsset();
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*peer);
    SetMockTime(GetTime<std::chrono::seconds>() + 61s);
    m_node.peerman->SendMessages(peer.get());
    BOOST_CHECK_EQUAL(CountGetData(*peer, inv), 1U);
    m_node.peerman->FinalizeNode(*peer);
}

BOOST_AUTO_TEST_CASE(consumed_announcement_is_not_requested_again)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto peer{MakeTestPeer(61)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    const auto vote{MakeVote()};
    const CInv inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};
    AnnounceInv(*m_node.peerman, *peer, inv);
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*peer);
    SendVote(*m_node.peerman, *peer, vote);
    BOOST_CHECK_EQUAL(CountMessages(*peer, NetMsgType::MNGOVERNANCESYNC), 0U);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 20);
    static_cast<ConnmanTestMsg&>(*m_node.connman).FlushSendBuffer(*peer);
    SetMockTime(GetTime<std::chrono::seconds>() + 61s);
    m_node.peerman->SendMessages(peer.get());
    BOOST_CHECK_EQUAL(CountGetData(*peer, inv), 0U);
    m_node.peerman->FinalizeNode(*peer);
}


BOOST_AUTO_TEST_CASE(independent_peers_authorize_one_response_each)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    auto first{MakeTestPeer(71)};
    auto second{MakeTestPeer(72)};
    m_node.peerman->InitializeNode(*first, NODE_NETWORK);
    m_node.peerman->InitializeNode(*second, NODE_NETWORK);
    const auto vote{MakeVote()};
    const CInv inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};
    AnnounceInv(*m_node.peerman, *first, inv);
    AnnounceInv(*m_node.peerman, *second, inv);
    SendVote(*m_node.peerman, *first, vote);
    SendVote(*m_node.peerman, *second, vote);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *first), 20);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *second), 20);
    // The same pending request cannot authorize a replay from either peer.
    SendVote(*m_node.peerman, *first, vote);
    SendVote(*m_node.peerman, *second, vote);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *first), 20);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *second), 20);
    m_node.peerman->FinalizeNode(*first);
    m_node.peerman->FinalizeNode(*second);
}

BOOST_AUTO_TEST_SUITE_END()
