// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_permissions.h>
#include <netaddress.h>
#include <netfulfilledman.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {
constexpr int64_t TIMEOUT{MASTERNODE_SYNC_TIMEOUT_SECONDS};
constexpr int64_t RECENT{MASTERNODE_SYNC_RECENT_TIP_SECONDS};
constexpr int64_t START{1'700'000'000};

// A real regtest chain below the last proof-of-work block, and a masternode sync reading it.
struct MasternodeSyncSetup : public RegTestingSetup {
    // Switching the stage asserts a loaded fulfilled-request store.
    CNetFulfilledRequestManager netfulfilledman;
    std::unique_ptr<CMasternodeSync> mn_sync;

    MasternodeSyncSetup()
    {
        BOOST_REQUIRE(netfulfilledman.LoadCache(/*load_cache=*/false));
        mn_sync = std::make_unique<CMasternodeSync>(*m_node.connman, netfulfilledman, *m_node.chainman);
        mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    }
    ~MasternodeSyncSetup() { SetMockTime(std::chrono::seconds{0}); }

    // A block with the given time on the active tip, the clock set to that time. Up to the last
    // proof-of-work block the target is the proof-of-work limit, whatever the time.
    CBlockIndex* MineAt(int64_t time)
    {
        SetMockTime(std::chrono::seconds{time});
        auto block = PrepareBlock(m_node, CScript{} << OP_TRUE);
        block->nTime = time;
        while (!CheckProofOfWork(block->GetHash(), block->nBits, Params().GetConsensus())) ++block->nNonce;
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(Params(), block, /*force_processing=*/true, /*new_block=*/nullptr));
        CBlockIndex* tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
        BOOST_REQUIRE_EQUAL(tip->GetBlockHash(), block->GetHash());
        return tip;
    }

    // The decision, under cs_main as TryEndBlockchainStage takes it.
    bool Done() { return WITH_LOCK(::cs_main, return mn_sync->IsBlockchainStageDone(TIMEOUT)); }
};

// A peer on 127.0.0.1, as the sync sees it: its port, how it is connected, its permissions.
std::unique_ptr<CNode> MakePeer(NodeId id, uint16_t port, ConnectionType type, NetPermissionFlags flags = NetPermissionFlags::None)
{
    in_addr ipv4{};
    ipv4.s_addr = htonl(0x7f000001);
    CNodeOptions options;
    options.permission_flags = flags;
    return std::make_unique<CNode>(id, /*sock=*/nullptr, CAddress{CService{CNetAddr{ipv4}, port}, NODE_NETWORK},
                                   /*nKeyedNetGroupIn=*/0, /*nLocalHostNonceIn=*/0, /*addrBindIn=*/CAddress{},
                                   /*addrNameIn=*/std::string{}, type, /*inbound_onion=*/false, std::move(options));
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(masternode_sync_tests, MasternodeSyncSetup)

// Above the last proof-of-work block a header costs nothing, so the blockchain stage also ends while the
// active tip is recent, and headers do not hold that back; once the tip is older, that way is closed.
BOOST_AUTO_TEST_CASE(recent_active_tip_ends_blockchain_stage)
{
    CBlockIndex* tip{MineAt(START)};
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_CHECK(Done());

    // Headers, however many: they postpone only the rule from Dash, which has not been met here.
    mn_sync->AcceptedBlockHeader(tip);
    mn_sync->NotifyHeaderTip(tip, /*fInitialDownload=*/false);
    BOOST_CHECK(Done());

    // The same tip, older than the window.
    SetMockTime(std::chrono::seconds{tip->GetBlockTime() + RECENT});
    BOOST_CHECK(!Done());

    // A new block makes the active tip recent again.
    MineAt(START + RECENT + 60);
    BOOST_CHECK(Done());
}

// The active tip is read when the stage is decided, under cs_main, and nothing is kept from a notification.
// InvalidateBlock() -- by hand, or from InstantSend when a lock conflicts with a mined transaction --
// disconnects block after block and notifies only at its end; until then the queued notification still
// names the block it rolled back. The decision sees the older tip it has left.
BOOST_AUTO_TEST_CASE(blockchain_stage_reads_the_tip_after_a_rollback)
{
    CBlockIndex* older{MineAt(START)};
    CBlockIndex* recent{MineAt(START + RECENT + 600)};
    // The notification the node delivers for `recent`, with a best header elsewhere (a header-only branch), so
    // that the rule from Dash stays out of it.
    mn_sync->UpdatedBlockTip(/*pindexTip=*/older, /*pindexNew=*/recent, /*fInitialDownload=*/false);
    SetMockTime(std::chrono::seconds{recent->GetBlockTime() + 60});
    BOOST_REQUIRE(Done());

    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, recent));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()), older);
    // `older` is RECENT + 660 s old: the stage holds, and the decision does not end it.
    BOOST_CHECK(!Done());
    BOOST_CHECK(!mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_BLOCKCHAIN);
}

// During initial block download a recent tip does not end the stage: importing blocks counts as such,
// whatever the tip's time.
BOOST_AUTO_TEST_CASE(no_recent_tip_during_initial_block_download)
{
    MineAt(START);
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_REQUIRE(Done());

    fImporting = true;
    static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).ResetIbd();
    const bool done_while_importing{Done()};
    fImporting = false;
    BOOST_CHECK(!done_while_importing);
    BOOST_CHECK(Done());
}

// For an older tip the rule from Dash is all there is, unchanged: with the best header reached, the stage
// ends once nothing has arrived for the timeout, and a header postpones that.
BOOST_AUTO_TEST_CASE(dash_rule_unchanged_for_an_older_tip)
{
    CBlockIndex* tip{MineAt(START)};
    SetMockTime(std::chrono::seconds{START + RECENT + 60});
    mn_sync->UpdatedBlockTip(/*pindexTip=*/tip, /*pindexNew=*/tip, /*fInitialDownload=*/false);
    BOOST_CHECK(!Done());
    SetMockTime(std::chrono::seconds{START + RECENT + 60 + TIMEOUT + 1});
    BOOST_CHECK(Done());

    mn_sync->AcceptedBlockHeader(tip);
    BOOST_CHECK(!Done());
    SetMockTime(std::chrono::seconds{START + RECENT + 60 + 2 * TIMEOUT + 2});
    BOOST_CHECK(Done());
}

// Leaving the blockchain stage is decided and made under cs_main and the stage lock, one stage forward. If
// `mnsync next` has already moved the stage, the decision does nothing: it does not move the stage again, which
// would run the governance stage's work (it takes network locks) under cs_main.
BOOST_AUTO_TEST_CASE(blockchain_stage_ends_in_one_step)
{
    MineAt(START);
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_REQUIRE(Done());

    // `mnsync next` first: the stage is governance, and the decision leaves it there.
    mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
    BOOST_CHECK(!mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);

    // Otherwise the decision moves exactly one stage, to governance.
    mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    BOOST_CHECK(mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
    BOOST_CHECK(!mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
}

// The end of the blockchain stage decided, then a Reset() from another thread (`mnsync reset`, a block
// notification), then the switch -- forced in that order. The Reset() must not be lost to a switch decided
// before it: it waits for the switch, then restarts the stage.
BOOST_AUTO_TEST_CASE(a_reset_is_not_lost_to_a_decided_switch)
{
    MineAt(START);
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_REQUIRE(Done());

    std::atomic<bool> reset_called{false};
    std::atomic<bool> reset_done{false};
    bool reset_called_between{false};
    bool reset_done_between{true};
    std::thread resetter;
    mn_sync->SetTestHookBeforeSwitch([&] {
        resetter = std::thread([&] {
            reset_called = true; // the race point: Reset() is called now, between the decision and the switch
            mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
            reset_done = true;
        });
        // The resetter has reached the call before the switch can go on ...
        for (int i = 0; i < 500 && !reset_called; ++i) UninterruptibleSleep(std::chrono::milliseconds{10});
        reset_called_between = reset_called;
        // ... and then every chance for the Reset() to finish here.
        for (int i = 0; i < 50 && !reset_done; ++i) UninterruptibleSleep(std::chrono::milliseconds{10});
        reset_done_between = reset_done;
    });
    const bool switched{mn_sync->TryEndBlockchainStage(TIMEOUT)};
    if (resetter.joinable()) resetter.join();
    mn_sync->SetTestHookBeforeSwitch(nullptr);

    BOOST_CHECK(switched);
    BOOST_CHECK(reset_called_between); // the Reset() was called between the decision and the switch
    BOOST_CHECK(!reset_done_between);  // and waited for the switch
    BOOST_CHECK(reset_done);
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_BLOCKCHAIN); // and then restarted the stage
}

// The governance stage's own decisions (its timeout, the end of the vote sync) are taken on the stage as read
// then. A Reset() afterwards restarts the blockchain stage, and the switch leaves that alone -- also once the
// restarted stage has come round to governance again, a new stage the decision never saw.
BOOST_AUTO_TEST_CASE(a_governance_decision_does_not_move_a_restarted_stage)
{
    mn_sync->SwitchToNextAsset();
    const CMasternodeSync::Stage seen{mn_sync->GetStage()};
    BOOST_REQUIRE_EQUAL(seen.asset, MASTERNODE_SYNC_GOVERNANCE);

    mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    BOOST_CHECK(!mn_sync->SwitchFrom(seen));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_BLOCKCHAIN);

    mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
    BOOST_CHECK(!mn_sync->SwitchFrom(seen));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);

    // A decision on the stage as it is now moves it, one step.
    BOOST_CHECK(mn_sync->SwitchFrom(mn_sync->GetStage()));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_FINISHED);
    BOOST_CHECK(!mn_sync->SwitchFrom(mn_sync->GetStage())); // nothing after finished
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_FINISHED);
}

// The vote sync's wait (every object asked for, then MASTERNODE_SYNC_TIMEOUT_SECONDS with hardly any new votes)
// belongs to one governance stage. A wait begun in a stage that a Reset() then ended does not carry over: the next
// governance stage waits its own time, and within one stage the wait ends as before.
BOOST_AUTO_TEST_CASE(vote_sync_wait_starts_anew_with_the_stage)
{
    SetMockTime(std::chrono::seconds{START});
    mn_sync->SwitchToNextAsset();
    const CMasternodeSync::Stage first{mn_sync->GetStage()};
    BOOST_REQUIRE_EQUAL(first.asset, MASTERNODE_SYNC_GOVERNANCE);
    BOOST_CHECK(!mn_sync->VoteSyncDone(first, /*tick=*/1, /*votes=*/0)); // the wait starts at START

    mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    mn_sync->SwitchToNextAsset();
    const CMasternodeSync::Stage second{mn_sync->GetStage()};
    BOOST_REQUIRE_EQUAL(second.asset, MASTERNODE_SYNC_GOVERNANCE);

    // Past the first stage's wait, but the second stage's own wait starts only now.
    SetMockTime(std::chrono::seconds{START + TIMEOUT + 1});
    BOOST_CHECK(!mn_sync->VoteSyncDone(second, /*tick=*/2, /*votes=*/0));
    SetMockTime(std::chrono::seconds{START + 2 * TIMEOUT + 2});
    BOOST_CHECK(mn_sync->VoteSyncDone(second, /*tick=*/3, /*votes=*/0));
}

// A trusted peer (noban, or a connection made by hand) is exempt from the sync's per-peer restrictions: once in
// this process everything fulfilled for it is forgotten (marks stored by an earlier run too), and once in each
// sync the "full-sync" mark of the sync before. The exemption used to be marked once per process only (a function
// static), so after a Reset() the trusted peer kept the last sync's mark and was disconnected as recently synced.
// What was asked of it within the expiry stays asked: asked again for the whole governance list sooner, the peer
// scores us as misbehaving. Any other peer keeps its marks, as before.
BOOST_AUTO_TEST_CASE(trusted_peer_restrictions_lifted_in_every_sync)
{
    const auto noban{MakePeer(0, 40000, ConnectionType::INBOUND, NetPermissionFlags::NoBan)};
    const auto manual{MakePeer(1, 40001, ConnectionType::MANUAL)};
    const auto plain{MakePeer(2, 40002, ConnectionType::INBOUND)};
    const std::vector<const CNode*> peers{noban.get(), manual.get(), plain.get()};
    const auto synced_from = [&](const CNode& peer) { return netfulfilledman.HasFulfilledRequest(peer.addr, "full-sync"); };
    const auto asked = [&](const CNode& peer) { return netfulfilledman.HasFulfilledRequest(peer.addr, "governance-sync"); };
    // What the end of a sync does to every connected peer (the governance stage's switch)
    const auto finish = [&] { for (const CNode* peer : peers) netfulfilledman.AddFulfilledRequest(peer->addr, "full-sync"); };
    // What the governance stage marks for every peer it asked for the list
    const auto ask = [&] { for (const CNode* peer : peers) netfulfilledman.AddFulfilledRequest(peer->addr, "governance-sync"); };

    // Marks stored by an earlier run: the first lifting in this process forgets them for the trusted peers.
    ask();

    // In the first sync the trusted peers once, the other never.
    BOOST_CHECK(mn_sync->LiftRestrictions(*noban));
    BOOST_CHECK(!asked(*noban));
    BOOST_CHECK(asked(*plain));
    BOOST_CHECK(mn_sync->LiftRestrictions(*manual));
    BOOST_CHECK(!mn_sync->LiftRestrictions(*plain));
    BOOST_CHECK(!mn_sync->LiftRestrictions(*noban));
    BOOST_CHECK(!mn_sync->LiftRestrictions(*manual));

    // It ends, every peer marked as synced from; within this sync the mark stays.
    finish();
    for (const CNode* peer : peers) {
        BOOST_CHECK(!mn_sync->LiftRestrictions(*peer));
        BOOST_CHECK(synced_from(*peer));
    }

    // Each new sync (a Reset(): `mnsync reset`, a block notification) lifts the trusted peers' restrictions again:
    // the last sync's "full-sync" mark goes, what they were asked for within the expiry stays; the other peer keeps
    // its marks.
    ask();
    for (int sync = 0; sync < 3; ++sync) {
        mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
        BOOST_CHECK(mn_sync->LiftRestrictions(*noban));
        BOOST_CHECK(mn_sync->LiftRestrictions(*manual));
        BOOST_CHECK(!mn_sync->LiftRestrictions(*plain));
        BOOST_CHECK(!synced_from(*noban));
        BOOST_CHECK(!synced_from(*manual));
        BOOST_CHECK(synced_from(*plain));
        for (const CNode* peer : peers) BOOST_CHECK(asked(*peer));
        finish();
    }
}

// The end of a sync marks every peer as synced from after its switch, outside the stage lock. If a Reset() comes
// in between and the new sync exempts a trusted peer before that mark, the mark of the finished sync must not land
// on it: the new sync would disconnect it as recently synced. Forced here: the hook runs between the governance
// stage's switch and its marking.
BOOST_AUTO_TEST_CASE(a_finish_overtaken_by_a_reset_marks_no_peer)
{
    // A connection manager of its own, holding the two peers the finish marks.
    ConnmanTestMsg connman{0x1337, 0x1337, *m_node.addrman, *m_node.netgroupman};
    CMasternodeSync sync{connman, netfulfilledman, *m_node.chainman};
    sync.Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    CNode* trusted{MakePeer(10, 40010, ConnectionType::MANUAL).release()};
    CNode* plain{MakePeer(11, 40011, ConnectionType::INBOUND).release()};
    for (CNode* peer : {trusted, plain}) {
        peer->fSuccessfullyConnected = true;
        connman.AddTestNode(*peer);
    }
    const auto synced_from = [&](const CNode& peer) { return netfulfilledman.HasFulfilledRequest(peer.addr, "full-sync"); };

    // A finish no Reset() overtakes marks every peer.
    BOOST_CHECK(sync.LiftRestrictions(*trusted));
    sync.SwitchToNextAsset(); // to the governance stage
    sync.SwitchToNextAsset(); // finished
    BOOST_REQUIRE(sync.IsSynced());
    BOOST_CHECK(synced_from(*trusted));
    BOOST_CHECK(synced_from(*plain));

    // The next sync lifts the trusted peer's mark; its finish is overtaken by a Reset() and the exemption of the
    // sync after it.
    sync.Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    BOOST_CHECK(sync.LiftRestrictions(*trusted));
    BOOST_CHECK(!synced_from(*trusted));
    sync.SwitchToNextAsset(); // to the governance stage
    bool exempted_between{false};
    sync.SetTestHookAfterSwitch([&] {
        sync.Reset(/*fForce=*/true, /*fNotifyReset=*/false);
        exempted_between = sync.LiftRestrictions(*trusted);
    });
    sync.SwitchToNextAsset(); // finished: the hook, then the marking
    sync.SetTestHookAfterSwitch(nullptr);
    BOOST_CHECK(exempted_between);
    BOOST_CHECK(!sync.IsSynced()); // the Reset() won
    BOOST_CHECK(!synced_from(*trusted)); // and the finished sync left no mark on the new one's trusted peer
    BOOST_CHECK(!sync.LiftRestrictions(*trusted)); // nor is there anything left to lift in this sync

    connman.ClearTestNodes();
}

// A trusted peer is never disconnected as recently synced, in whatever order a finish, a Reset() and the
// exemption run. The order review r4 found: a tick lifts the restrictions, a finish of the same sync marks every
// peer (rightly: same generation), a Reset() starts the next sync, and only then the tick decides on the mark,
// before the next sync's exemption. Any other peer is disconnected for the mark, as before.
BOOST_AUTO_TEST_CASE(a_trusted_peer_is_never_disconnected_as_recently_synced)
{
    const auto manual{MakePeer(20, 40020, ConnectionType::MANUAL)};
    const auto noban{MakePeer(21, 40021, ConnectionType::INBOUND, NetPermissionFlags::NoBan)};
    const auto plain{MakePeer(22, 40022, ConnectionType::INBOUND)};
    const std::vector<const CNode*> peers{manual.get(), noban.get(), plain.get()};

    for (const CNode* peer : peers) mn_sync->LiftRestrictions(*peer);
    for (const CNode* peer : peers) netfulfilledman.AddFulfilledRequest(peer->addr, "full-sync"); // the finish
    mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    BOOST_CHECK(!mn_sync->DisconnectsAsRecentlySynced(*manual));
    BOOST_CHECK(!mn_sync->DisconnectsAsRecentlySynced(*noban));
    BOOST_CHECK(mn_sync->DisconnectsAsRecentlySynced(*plain));

    // The next sync's exemption then lifts the mark, and the trusted peers are asked again.
    BOOST_CHECK(mn_sync->LiftRestrictions(*manual));
    BOOST_CHECK(mn_sync->LiftRestrictions(*noban));
    BOOST_CHECK(!netfulfilledman.HasFulfilledRequest(manual->addr, "full-sync"));
    BOOST_CHECK(!netfulfilledman.HasFulfilledRequest(noban->addr, "full-sync"));
    BOOST_CHECK(mn_sync->DisconnectsAsRecentlySynced(*plain));
}

BOOST_AUTO_TEST_SUITE_END()
