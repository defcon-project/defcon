// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/pose_service.h>
#include <evo/pose_service_manager.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_sentinels.h>
#include <key.h>
#include <llmq/context.h>
#include <llmq/options.h>
#include <llmq/signing.h>
#include <masternode/meta.h>
#include <masternode/node.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <vector>
#include <future>
#include <limits>
#include <memory>

namespace {
//! Registered after the PeerManager, so it sees each tip event after it in both rounds: what the manager's epoch is
//! in this observer's UpdatedBlockTip (before the tick, since SF1-A) and in its UpdatedBlockTipFinal (after it).
class TipOrderObserver final : public CValidationInterface
{
public:
    explicit TipOrderObserver(const dsl::CPoSeServiceManager& dslman) : m_dslman(dslman) {}
    std::vector<std::pair<int, uint32_t>> first; //!< (height, epoch) in UpdatedBlockTip
    std::vector<std::pair<int, uint32_t>> last;  //!< (height, epoch) in UpdatedBlockTipFinal
    std::function<void()> during_event;          //!< run in this observer's UpdatedBlockTip

protected:
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex*, bool) override
    {
        first.emplace_back(pindexNew->nHeight, m_dslman.CurrentEpoch());
        if (during_event) during_event();
    }
    void UpdatedBlockTipFinal(const CBlockIndex* pindexNew, const CBlockIndex*, bool) override
    {
        last.emplace_back(pindexNew->nHeight, m_dslman.CurrentEpoch());
    }

private:
    const dsl::CPoSeServiceManager& m_dslman;
};

//! Moves a separate deterministic-MN reader's tip in each tip event, where production's CDSNotificationInterface
//! moves the live manager's: in the event's first round, so before the tick, which runs after every subscriber's
//! UpdatedBlockTip.
class ReaderTipFollower final : public CValidationInterface
{
public:
    explicit ReaderTipFollower(CDeterministicMNManager& dmnman) : m_dmnman(dmnman) {}

protected:
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex*, bool) override
    {
        m_dmnman.UpdatedBlockTip(pindexNew);
    }

private:
    CDeterministicMNManager& m_dmnman;
};

//! The announcements read and dropped unread over the measurement window, which this takes (and so restarts).
struct ResponseCounts {
    uint64_t read{0};
    uint64_t unread{0};
};
ResponseCounts TakeResponseCounts()
{
    ResponseCounts counts;
    for (const auto& slot : dsl::TakePerfSnapshot().epochs) {
        if (!slot.used) continue;
        counts.read += slot.values[static_cast<size_t>(dsl::PerfMetric::DECODE_RESPONSE)].count;
        counts.unread += slot.values[static_cast<size_t>(dsl::PerfMetric::DUPLICATE_RESPONSE_UNDECODED)].count;
    }
    return counts;
}

constexpr int ACTIVATION_HEIGHT{240}; // epoch 10, beyond an unstarted manager's retained window
constexpr int NEXT_EPOCH_HEIGHT{264}; // epoch 11, activation gate already open

// Block the real validation queue on its scheduler thread, without sleeps or
// wallet locks. Always release it before the fixture tears down, also on failure.
class BlockValidationQueue {
    std::promise<void> m_ready;
    std::promise<void> m_release;
    std::future<void> m_release_future{m_release.get_future()};
    bool m_released{false};
public:
    BlockValidationQueue()
    {
        auto ready_future = m_ready.get_future();
        CallFunctionInValidationInterfaceQueue([this] {
            m_ready.set_value();
            m_release_future.wait();
        });
        ready_future.wait();
    }
    void Release()
    {
        if (m_released) return;
        m_released = true;
        m_release.set_value();
        SyncWithValidationInterfaceQueue();
    }
    ~BlockValidationQueue() { Release(); }
};

struct DSLActivationSetup : RegTestingSetup {
    CBLSSecretKey operator_key;
    uint256 protx{uint256::ONE};
    const CBlockIndex* base{nullptr};
    // Separate readers/managers for message processing: the chainstate, quorum
    // and CoinJoin objects keep their original live deterministic-MN manager.
    std::unique_ptr<CDeterministicMNManager> dmnman;
    std::unique_ptr<dsl::CPoSeServiceManager> dslman{std::make_unique<dsl::CPoSeServiceManager>()};
    //! Set by BecomeMasternode(); declared before the PeerManager, which points to it, so it outlives it.
    std::unique_ptr<CActiveMasternodeManager> activeman;
    std::unique_ptr<PeerManager> peerman;
    std::vector<CBLSSecretKey> operator_keys;
    std::unique_ptr<CNode> peer;
    std::unique_ptr<CNode> other_peer;
    bool previous_perf{dsl::PerfEnabled()};

    DSLActivationSetup() : RegTestingSetup{{"-testactivationheight=dsl@240", "-dip3params=2:2"}}
    {
        dsl::SetPerfEnabled(false);
        for (int height = 1; height < ACTIVATION_HEIGHT; ++height) {
            MineBlock(m_node, CScript{} << OP_TRUE);
        }
        SyncWithValidationInterfaceQueue();
        static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).JumpOutOfIbd();
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(false));
        BOOST_REQUIRE(m_node.mn_metaman->LoadCache(false));
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());
        dmnman = std::make_unique<CDeterministicMNManager>(m_node.chainman->ActiveChainstate(), *m_node.evodb);
        CreatePeerManager();
        peerman->SetBestHeight(ACTIVATION_HEIGHT - 1);
        RegisterValidationInterface(peerman.get());
        peer = MakeTestPeer(0);
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*peer, NODE_NETWORK);
    }

    ~DSLActivationSetup()
    {
        SyncWithValidationInterfaceQueue();
        UnregisterValidationInterface(peerman.get());
        if (other_peer) peerman->FinalizeNode(*other_peer);
        peerman->FinalizeNode(*peer);
        dsl::SetPerfEnabled(previous_perf);
        dsl::TakePerfSnapshot();
    }

    void CreatePeerManager()
    {
        peerman = PeerManager::make(Params(), *m_node.connman, *m_node.addrman, m_node.banman.get(),
                                   *m_node.chainman, *m_node.mempool, *m_node.mn_metaman, *m_node.mn_sync,
                                   *m_node.govman, *m_node.sporkman, activeman.get(), dmnman,
                                   m_node.cj_ctx, m_node.llmq_ctx, dslman, false);
    }

    //! From here on the PeerManager is a masternode's, MN#1's (InstallList gives MN#1 the first operator key):
    //! ProcessDSLTick's duties after its masternode check, such as relaying a signed commitment, are a member's.
    //! Called before the activation block, so the new PeerManager's first tick opens its epoch.
    void BecomeMasternode()
    {
        BOOST_REQUIRE(operator_keys.empty());
        SyncWithValidationInterfaceQueue();
        UnregisterValidationInterface(peerman.get());
        peerman->FinalizeNode(*peer);
        CBLSSecretKey key;
        key.MakeNewKey();
        operator_keys.push_back(key);
        activeman = std::make_unique<CActiveMasternodeManager>(key, *m_node.connman, dmnman);
        CreatePeerManager();
        peerman->SetBestHeight(ACTIVATION_HEIGHT - 1);
        RegisterValidationInterface(peerman.get());
        peer = MakeTestPeer(0);
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*peer, NODE_NETWORK);
    }

    //! Writes the synthetic list of `count` masternodes as the snapshot of the block `block_hash` at `height`. Called
    //! before that block connects (ConnectBase): from SF1-L on a masternode's tick reads the epoch base's list on the
    //! base block itself, and this reader would keep whatever it found there.
    void InstallList(const uint256& block_hash, int height, size_t count)
    {
        BOOST_REQUIRE_LE(count, std::numeric_limits<uint32_t>::max());
        CDeterministicMNList list{block_hash, height, static_cast<uint32_t>(count)};
        for (size_t i = 0; i < count; ++i) {
            if (operator_keys.size() <= i) {
                CBLSSecretKey key;
                key.MakeNewKey();
                operator_keys.push_back(key);
            }
            const auto& key = operator_keys[i];
            auto mn = std::make_shared<CDeterministicMN>(i);
            mn->proTxHash = uint256S(std::to_string(i + 1));
            mn->collateralOutpoint = COutPoint{mn->proTxHash, 0};
            auto state = std::make_shared<CDeterministicMNState>();
            state->nRegisteredHeight = 1;
            CKey owner_key;
            owner_key.MakeNewKey(true);
            state->keyIDOwner = owner_key.GetPubKey().GetID();
            state->pubKeyOperator.Set(key.GetPublicKey(), /*specificLegacyScheme=*/false);
            mn->pdmnState = state;
            list.AddMN(mn);
        }
        operator_key = operator_keys.front();
        // No component's existing manager is replaced or freed, and mining uses
        // the original. The synthetic snapshot is shared through evodb with those
        // managers; the block's own connection commits it.
        m_node.evodb->Write(std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), block_hash), list);
    }

    //! A block on the tip, solved but not connected (MineBlock without its ProcessNewBlock).
    std::shared_ptr<CBlock> SolvedBlock(const CScript& script)
    {
        auto block = PrepareBlock(m_node, script);
        while (!CheckProofOfWork(block->GetHash(), block->nBits, Params().GetConsensus())) {
            ++block->nNonce;
            BOOST_REQUIRE(block->nNonce != 0);
        }
        return block;
    }

    //! Connects `block` as the epoch base at `height` with its synthetic list of `count` masternodes in place first.
    void ConnectBase(const std::shared_ptr<CBlock>& block, int height, size_t count)
    {
        // ActivateBestChain would wait for the blocked queue above ten callbacks.
        BOOST_REQUIRE_LE(GetMainSignals().CallbacksPending(), 10);
        InstallList(block->GetHash(), height, count);
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(Params(), block, true, nullptr));
        base = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE_EQUAL(base->nHeight, height);
        BOOST_REQUIRE(base->GetBlockHash() == block->GetHash());
        dmnman->UpdatedBlockTip(base); // production CDSNotificationInterface's synchronous update
        BOOST_REQUIRE_EQUAL(dmnman->GetListForBlock(base).GetAllMNsCount(), count);
    }

    void ConnectActivationBlock(size_t count = 1, const CScript& script = CScript{} << OP_TRUE)
    {
        ConnectBase(SolvedBlock(script), ACTIVATION_HEIGHT, count);
    }

    dsl::CPoSeServiceResponse Response(const uint256& hash) const
    {
        dsl::CPoSeServiceManager sender;
        sender.BeginEpoch(ACTIVATION_HEIGHT / Params().GetConsensus().nDSLEpochInterval, hash);
        return sender.AnnounceLiveness(protx, operator_key);
    }

    void Send(const dsl::CPoSeServiceResponse& response, CNode& from)
    {
        CDataStream payload{SER_NETWORK, PROTOCOL_VERSION};
        payload << response;
        LOCK(NetEventsInterface::g_msgproc_mutex);
        SendMessage(*peerman, from, NetMsgType::POSERESPONSE, std::move(payload));
    }

    void Send(const dsl::CPoSeServiceResponse& response) { Send(response, *peer); }

    //! An announcement payload as given, which need not be any announcement's serialization.
    void SendRaw(CDataStream payload, CNode& from)
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        SendMessage(*peerman, from, NetMsgType::POSERESPONSE, std::move(payload));
    }

    // Mine normally to 263, then solve the actual next base before connecting
    // it so a genuine pre-base response can sign its final hash. Each callback
    // is drained here; the test blocks only the base-264 tick below.
    std::shared_ptr<CBlock> PrepareNextEpochBase()
    {
        ConnectActivationBlock(2);
        SyncWithValidationInterfaceQueue();
        for (int height = ACTIVATION_HEIGHT + 1; height < NEXT_EPOCH_HEIGHT; ++height) {
            MineBlock(m_node, CScript{} << OP_TRUE);
            SyncWithValidationInterfaceQueue();
        }
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), 10U);
        return SolvedBlock(CScript{} << OP_TRUE);
    }

    void ConnectNextEpochBase(const std::shared_ptr<CBlock>& block) { ConnectBase(block, NEXT_EPOCH_HEIGHT, 2); }

    dsl::CPoSeServiceResponse NextEpochResponse(const uint256& hash, size_t mn_index) const
    {
        dsl::CPoSeServiceManager sender;
        sender.BeginEpoch(NEXT_EPOCH_HEIGHT / Params().GetConsensus().nDSLEpochInterval, hash);
        return sender.AnnounceLiveness(uint256S(std::to_string(mn_index + 1)), operator_keys.at(mn_index));
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dsl_activation_tests, DSLActivationSetup)

BOOST_AUTO_TEST_CASE(response_survives_a_blocked_activation_tick)
{
    BlockValidationQueue blocked;
    ConnectActivationBlock();
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 0U);
    Send(Response(base->GetBlockHash()));
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 0U); // unstarted epoch; release below proves retention
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
    BOOST_CHECK(dslman->HasResponded(protx));
}

BOOST_AUTO_TEST_CASE(response_survives_best_height_before_begin_epoch)
{
    BlockValidationQueue blocked;
    ConnectActivationBlock();
    // Isolate S2: even the old asynchronous activation gate now admits it.
    peerman->SetBestHeight(ACTIVATION_HEIGHT);
    Send(Response(base->GetBlockHash()));
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 0U);
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
    // A later duplicate must still not increase the responding set.
    Send(Response(base->GetBlockHash()));
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(held_response_still_requires_the_epoch_base_signature)
{
    BlockValidationQueue blocked;
    ConnectActivationBlock();
    Send(Response(uint256::TWO)); // well-formed signature for the wrong chain
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 0U);
    Send(Response(base->GetBlockHash()));
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(synchronous_rewind_closes_the_gate_before_the_queued_tip)
{
    BlockValidationQueue blocked;
    ConnectActivationBlock();
    peerman->SetBestHeight(ACTIVATION_HEIGHT);
    // Exercise the same synchronous signal used by invalidation/reorgs. The
    // asynchronous height still names the activated tip; it must not admit a
    // payload after this signal has moved the activation height back below it.
    {
        LOCK(cs_main);
        GetMainSignals().SynchronousUpdatedBlockTip(base->pprev, nullptr, /*fInitialDownload=*/false);
    }
    CDataStream payload{SER_NETWORK, PROTOCOL_VERSION};
    payload << Response(base->GetBlockHash());
    const auto size = payload.size();
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        const std::atomic<bool> interrupt{false};
        peerman->ProcessMessage(*peer, NetMsgType::POSERESPONSE, payload, std::chrono::microseconds{0}, interrupt);
    }
    BOOST_CHECK_EQUAL(payload.size(), size);
    {
        LOCK(cs_main);
        GetMainSignals().SynchronousUpdatedBlockTip(base, nullptr, /*fInitialDownload=*/false);
    }
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 0U);
}

BOOST_AUTO_TEST_CASE(activation_gate_rejects_unread_and_records_diagnostics)
{
    dsl::TakePerfSnapshot();
    dsl::SetPerfEnabled(true);
    CDataStream payload{SER_NETWORK, PROTOCOL_VERSION};
    payload << uint64_t{123}; // intentionally not a readable response
    const auto size = payload.size();
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        const std::atomic<bool> interrupt{false};
        peerman->ProcessMessage(*peer, NetMsgType::POSERESPONSE, payload, std::chrono::microseconds{0}, interrupt);
    }
    BOOST_CHECK_EQUAL(payload.size(), size); // activation is still before decoding and quota
    BOOST_CHECK_EQUAL(MisbehaviorScore(*peerman, *peer), 0);
    const auto snapshot = dsl::TakePerfSnapshot();
    uint64_t refused{0};
    for (const auto& epoch : snapshot.epochs) {
        refused += epoch.values[static_cast<size_t>(dsl::PerfMetric::ACTIVATION_REFUSED)].count;
    }
    BOOST_CHECK_EQUAL(refused, 1U);
}

BOOST_AUTO_TEST_CASE(activation_burst_exceeds_the_minimum_hold_without_loss)
{
    constexpr size_t count{4097}; // above the 4096-entry floor; list-sized hold is 16384
    BlockValidationQueue blocked;
    ConnectActivationBlock(count);
    BOOST_REQUIRE(!peer->HasPermission(NetPermissionFlags::NoBan));
    dsl::CPoSeServiceManager sender;
    sender.BeginEpoch(ACTIVATION_HEIGHT / Params().GetConsensus().nDSLEpochInterval, base->GetBlockHash());
    for (size_t i = 0; i < count; ++i) {
        Send(sender.AnnounceLiveness(uint256S(std::to_string(i + 1)), operator_keys[i]));
    }
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 0U);
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), count);
}

BOOST_AUTO_TEST_CASE(rebase_does_not_taint_the_relayer_of_the_new_base)
{
    BlockValidationQueue blocked;
    ConnectActivationBlock();
    const CBlockIndex* old_base{base};
    Send(Response(old_base->GetBlockHash()));
    BOOST_REQUIRE_LE(GetMainSignals().CallbacksPending(), 10);
    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, const_cast<CBlockIndex*>(old_base)));
    ConnectActivationBlock(1, CScript{} << OP_TRUE << OP_TRUE);
    BOOST_REQUIRE(base->GetBlockHash() != old_base->GetBlockHash());
    Send(Response(base->GetBlockHash())); // same honest relayer, distinct active-base signature
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->CurrentEpochHash(), base->GetBlockHash());
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(restart_at_activation_seeds_the_gate_without_a_new_signal)
{
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(peerman.get());
    peerman->FinalizeNode(*peer);
    peerman.reset();
    dslman = std::make_unique<dsl::CPoSeServiceManager>();
    CreatePeerManager(); // connected height 240; no signal or SetBestHeight follows
    RegisterValidationInterface(peerman.get());
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*peer, NODE_NETWORK);
    }
    Send(Response(base->GetBlockHash()));
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 0U);
    // A normal later tick drains the admitted response; no block is connected.
    {
        LOCK(cs_main);
        GetMainSignals().UpdatedBlockTip(base, base->pprev, false);
    }
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(late_old_base_response_does_not_taint_the_new_base_relayer)
{
    BlockValidationQueue blocked;
    ConnectActivationBlock();
    const auto old_response = Response(base->GetBlockHash());
    BOOST_REQUIRE_LE(GetMainSignals().CallbacksPending(), 10);
    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, const_cast<CBlockIndex*>(base)));
    ConnectActivationBlock(1, CScript{} << OP_TRUE << OP_TRUE);
    // Both arrive after the new base is connected. The receiver's observed
    // base cannot tell which base an in-flight response was signed against.
    Send(old_response);
    Send(Response(base->GetBlockHash()));
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(connected_response_survives_prebase_relayer_taint)
{
    const auto block = PrepareNextEpochBase();
    BlockValidationQueue blocked;
    Send(NextEpochResponse(uint256::TWO, 0)); // unverified pre-base MN#1, same relayer
    ConnectNextEpochBase(block);
    Send(NextEpochResponse(block->GetHash(), 1)); // verified connected-base MN#2
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 10U);
    blocked.Release();
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 11U);
    BOOST_CHECK(!dslman->HasResponded(uint256::ONE));
    BOOST_CHECK(dslman->HasResponded(uint256::TWO));
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(connected_duplicate_verifies_and_promotes_prebase_response)
{
    const auto block = PrepareNextEpochBase();
    const auto genuine = NextEpochResponse(block->GetHash(), 1);
    BlockValidationQueue blocked;
    Send(NextEpochResponse(uint256::TWO, 0)); // later taints this relayer at drain
    Send(genuine); // null-tagged because the base is not connected yet
    ConnectNextEpochBase(block);
    Send(genuine); // must verify/promote the incumbent before vouching
    blocked.Release();
    BOOST_CHECK(!dslman->HasResponded(uint256::ONE));
    BOOST_CHECK(dslman->HasResponded(uint256::TWO));
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(invalid_connected_duplicate_does_not_vouch_for_prebase_entry)
{
    const auto block = PrepareNextEpochBase();
    other_peer = MakeTestPeer(1);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*other_peer, NODE_NETWORK);
    }
    const auto invalid = NextEpochResponse(uint256::TWO, 0);
    BlockValidationQueue blocked;
    Send(invalid, *other_peer); // one invalid pre-base entry, relayer A
    Send(NextEpochResponse(block->GetHash(), 1)); // genuine pre-base MN#2, relayer C
    ConnectNextEpochBase(block);
    Send(invalid); // C's late old-base copy must not become another voucher
    blocked.Release();
    BOOST_CHECK(!dslman->HasResponded(uint256::ONE));
    BOOST_CHECK(dslman->HasResponded(uint256::TWO));
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(the_tick_runs_after_the_other_subscribers_of_its_event)
{
    TipOrderObserver observer{*dslman};
    RegisterValidationInterface(&observer);
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(&observer);
    // The activation block's tick begins epoch 10: after every subscriber's UpdatedBlockTip of that event, and
    // within the event, before its last round reaches this observer.
    BOOST_REQUIRE(!observer.first.empty() && !observer.last.empty());
    BOOST_CHECK(observer.first.back() == std::make_pair(ACTIVATION_HEIGHT, uint32_t{0}));
    BOOST_CHECK(observer.last.back() == std::make_pair(ACTIVATION_HEIGHT, uint32_t{10}));
}

BOOST_AUTO_TEST_CASE(queued_events_finish_with_their_ticks_in_chain_order)
{
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    for (int height = ACTIVATION_HEIGHT + 1; height < NEXT_EPOCH_HEIGHT - 1; ++height) {
        MineBlock(m_node, CScript{} << OP_TRUE);
        SyncWithValidationInterfaceQueue();
    }
    TipOrderObserver observer{*dslman};
    RegisterValidationInterface(&observer);
    {
        // [U1, U2, SYNC]: two tip events queued behind a parked queue; the sync after them returns only once both
        // events, each with its own tick, have finished, the boundary block's last.
        BlockValidationQueue blocked;
        MineBlock(m_node, CScript{} << OP_TRUE); // NEXT_EPOCH_HEIGHT - 1, epoch 10
        MineBlock(m_node, CScript{} << OP_TRUE); // NEXT_EPOCH_HEIGHT, epoch 11's base
        BOOST_CHECK(observer.last.empty());
        blocked.Release(); // and SyncWithValidationInterfaceQueue
    }
    UnregisterValidationInterface(&observer);
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 11U);
    BOOST_REQUIRE_EQUAL(observer.last.size(), 2U);
    BOOST_CHECK(observer.first[0] == std::make_pair(NEXT_EPOCH_HEIGHT - 1, uint32_t{10}));
    BOOST_CHECK(observer.last[0] == std::make_pair(NEXT_EPOCH_HEIGHT - 1, uint32_t{10}));
    BOOST_CHECK(observer.first[1] == std::make_pair(NEXT_EPOCH_HEIGHT, uint32_t{10}));
    BOOST_CHECK(observer.last[1] == std::make_pair(NEXT_EPOCH_HEIGHT, uint32_t{11}));
}

// R14: a subscriber after the PeerManager resets the masternode sync in the same event, as
// CDSNotificationInterface can (CMasternodeSync::UpdatedBlockTip -> Reset(true)). The tick runs with the sync the
// PeerManager found, where the tick used to run, so it still begins the epoch.
BOOST_AUTO_TEST_CASE(the_tick_keeps_the_sync_state_its_event_found)
{
    TipOrderObserver observer{*dslman};
    observer.during_event = [this] { m_node.mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false); };
    RegisterValidationInterface(&observer);
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(&observer);
    BOOST_CHECK(!m_node.mn_sync->IsBlockchainSynced()); // the reset took place in the event
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 10U);     // and the tick ran as noted before it
    BOOST_CHECK(observer.last.back() == std::make_pair(ACTIVATION_HEIGHT, uint32_t{10}));
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_CHECK(m_node.mn_sync->IsBlockchainSynced());
}

BOOST_AUTO_TEST_CASE(an_accepted_announcement_s_copies_are_dropped_unread)
{
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), 10U);
    other_peer = MakeTestPeer(1);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*other_peer, NODE_NETWORK);
    }
    dsl::SetPerfEnabled(true);
    dsl::TakePerfSnapshot();
    const auto response = Response(base->GetBlockHash());
    Send(response); // read and accepted
    BOOST_REQUIRE_EQUAL(dslman->RespondedCount(), 1U);
    Send(response);
    Send(response, *other_peer);
    auto counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 1U);   // only the first was read
    BOOST_CHECK_EQUAL(counts.unread, 2U); // its two byte-identical copies were not
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
    // Other bytes take the ordinary path and are read: here the same announcement with damaged signature bytes,
    // which this node never accepted.
    CDataStream damaged{SER_NETWORK, PROTOCOL_VERSION};
    damaged << response;
    BOOST_REQUIRE_EQUAL(damaged.size(), dsl::CPoSeServiceResponse::WIRE_SIZE);
    damaged[damaged.size() - 1] = std::byte(std::to_integer<uint8_t>(damaged[damaged.size() - 1]) ^ 0x5a);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        SendMessage(*peerman, *other_peer, NetMsgType::POSERESPONSE, std::move(damaged));
    }
    counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 1U);
    BOOST_CHECK_EQUAL(counts.unread, 0U);
    // An unreadable payload never matches: one byte short, it is read, refused and scored as before.
    CDataStream truncated{SER_NETWORK, PROTOCOL_VERSION};
    truncated << response;
    truncated.resize(truncated.size() - 1);
    {
        ASSERT_DEBUG_LOG("unreadable poseresp");
        LOCK(NetEventsInterface::g_msgproc_mutex);
        SendMessage(*peerman, *other_peer, NetMsgType::POSERESPONSE, std::move(truncated));
    }
    counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 1U);
    BOOST_CHECK_EQUAL(counts.unread, 0U);
    BOOST_CHECK_EQUAL(dslman->RespondedCount(), 1U);
}

BOOST_AUTO_TEST_CASE(a_held_announcement_s_copies_vouch_unread)
{
    const auto block = PrepareNextEpochBase();
    other_peer = MakeTestPeer(1);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*other_peer, NODE_NETWORK);
    }
    dsl::SetPerfEnabled(true);
    dsl::TakePerfSnapshot();
    const auto genuine = NextEpochResponse(block->GetHash(), 1);
    {
        // One block before its base: read and held, then a byte-identical copy from another peer vouches unread.
        ASSERT_DEBUG_LOG("arrived 1 block(s) before its base block, already held, vouched for by 2 peer(s), peer=");
        Send(genuine);
        Send(genuine, *other_peer);
    }
    {
        // And again from the same peer: unread, and it stays one voucher.
        ASSERT_DEBUG_LOG("already held, vouched for by 2 peer(s), peer=");
        Send(genuine, *other_peer);
    }
    const auto counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 1U);
    BOOST_CHECK_EQUAL(counts.unread, 2U);
    ConnectNextEpochBase(block);
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK(dslman->HasResponded(uint256::TWO)); // the drain accepted it
}

BOOST_AUTO_TEST_CASE(a_connected_base_s_held_copy_vouches_unread)
{
    const auto block = PrepareNextEpochBase();
    other_peer = MakeTestPeer(1);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*other_peer, NODE_NETWORK);
    }
    const auto genuine = NextEpochResponse(block->GetHash(), 1);
    BlockValidationQueue blocked;
    ConnectNextEpochBase(block); // the base is connected, its tick still queued
    dsl::SetPerfEnabled(true);
    dsl::TakePerfSnapshot();
    {
        ASSERT_DEBUG_LOG("arrived before its local epoch tick, already held, vouched for by 2 peer(s), peer=");
        Send(genuine);              // read, verified for the connected base and held
        Send(genuine, *other_peer); // the same bytes for the same base: vouches unread
    }
    const auto counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 1U);
    BOOST_CHECK_EQUAL(counts.unread, 1U);
    blocked.Release();
    BOOST_CHECK(dslman->HasResponded(uint256::TWO));
}

BOOST_AUTO_TEST_CASE(the_record_of_accepted_bytes_is_bound_to_its_base)
{
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    const auto response = Response(base->GetBlockHash());
    Send(response);
    BOOST_REQUIRE_EQUAL(dslman->RespondedCount(), 1U);
    const uint256 hash = dsl::ResponseWireHash(response);
    CDataStream wire{SER_NETWORK, PROTOCOL_VERSION};
    wire << response;
    const auto peeked = dsl::PeekResponseWire(MakeUCharSpan(wire));
    BOOST_REQUIRE(peeked);
    BOOST_CHECK(peeked->hash == hash); // the decoded announcement's hash is its wire bytes' hash
    BOOST_CHECK_EQUAL(peeked->nEpoch, response.nEpoch);
    BOOST_CHECK(peeked->proTxHash == response.proTxHash);
    const uint32_t epoch = response.nEpoch;
    BOOST_CHECK(dslman->HaveIdenticalResponse(epoch, base->GetBlockHash(), protx, hash));
    BOOST_CHECK(!dslman->HaveIdenticalResponse(epoch, uint256::TWO, protx, hash));      // another base
    BOOST_CHECK(!dslman->HaveIdenticalResponse(epoch, base->GetBlockHash(), uint256::TWO, hash)); // another masternode
    BOOST_CHECK(!dslman->HaveIdenticalResponse(epoch, base->GetBlockHash(), protx, uint256::ONE)); // other bytes
    wire << uint8_t{0};
    BOOST_CHECK(!dsl::PeekResponseWire(MakeUCharSpan(wire))); // another length is not peeked at all
    // A shallow reorg swapping the epoch's base clears what was accepted on the old one.
    BOOST_CHECK(dslman->BeginEpoch(epoch, uint256::TWO) == dsl::CPoSeServiceManager::EpochChange::Rebased);
    BOOST_CHECK(!dslman->HaveIdenticalResponse(epoch, base->GetBlockHash(), protx, hash));
}

// R14 at the end of an epoch (sentinelfix.md 6.1): in the event of the epoch's last block (E/23) a subscriber after the
// PeerManager resets the masternode sync, and the tick, running with the sync its event found, still relays the
// commitment the quorum signed -- as before SF1-A, where the tick ran ahead of the reset. Relaying is a member's duty,
// so the PeerManager here is a masternode's.
BOOST_AUTO_TEST_CASE(the_last_tick_relays_the_signed_commitment_across_a_sync_reset)
{
    BecomeMasternode();
    ConnectActivationBlock();
    SyncWithValidationInterfaceQueue();
    BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), 10U);
    for (int height = ACTIVATION_HEIGHT + 1; height < NEXT_EPOCH_HEIGHT - 1; ++height) {
        MineBlock(m_node, CScript{} << OP_TRUE);
        SyncWithValidationInterfaceQueue();
    }
    // What a member keeps once it asked the quorum to sign, and the signature the quorum recovered for it, written
    // directly: the signing manager keeps only verified ones (instantsend_queue_tests does the same). This chain has
    // no quorum, so the tick never asks to sign; from position 21 on it looks for the signature, which is here only
    // for the last block's event.
    const auto llmq_type = llmq::GetChainLocksLLMQType(Params().GetConsensus(), NEXT_EPOCH_HEIGHT);
    CPoSeServiceCommitment commitment;
    commitment.nEpoch = 10;
    commitment.epochBlockHash = base->GetBlockHash();
    commitment.llmqType = llmq_type;
    commitment.quorumHash = uint256::ONE;
    commitment.missed.assign(1, false);
    commitment.observed.assign(1, true);
    const uint256 msg_hash{uint256::TWO};
    dslman->RememberSigningCandidate(commitment, msg_hash);
    CBLSSecretKey quorum_key;
    quorum_key.MakeNewKey();
    auto& sigman = *m_node.llmq_ctx->sigman;
    const auto recovered = std::make_shared<const llmq::CRecoveredSig>(
        llmq_type, commitment.quorumHash, commitment.GetRequestId(), msg_hash,
        quorum_key.Sign(msg_hash, /*specificLegacyScheme=*/false));
    sigman.ProcessRecoveredSig(recovered, *peerman);
    BOOST_REQUIRE(sigman.HasRecoveredSig(llmq_type, commitment.GetRequestId(), msg_hash));
    BOOST_REQUIRE(!dslman->HasSignedCommitment(10, msg_hash)); // only a tick relays it
    TipOrderObserver observer{*dslman};
    observer.during_event = [this] { m_node.mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false); };
    RegisterValidationInterface(&observer);
    MineBlock(m_node, CScript{} << OP_TRUE); // NEXT_EPOCH_HEIGHT - 1, position 23 of epoch 10
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(&observer);
    BOOST_CHECK(!m_node.mn_sync->IsBlockchainSynced());     // the reset took place in the event
    BOOST_CHECK(dslman->HasSignedCommitment(10, msg_hash)); // and the tick relayed the commitment the quorum signed
    BOOST_REQUIRE(!observer.last.empty());
    BOOST_CHECK(observer.last.back() == std::make_pair(NEXT_EPOCH_HEIGHT - 1, uint32_t{10}));
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_CHECK(m_node.mn_sync->IsBlockchainSynced());
}

// SF1-L: on every block of the epoch the tick speaks for it from the epoch base's list, the one the receivers and
// the aggregation judge against. Until SF1-L it took the chain tip's list, which from the block after the base on is
// another block's list (and, after a PoSe ban, ProUpRevTx, a revival, a new confirmation or a spent collateral, names
// other sentinels). Here the base's tick is skipped (masternode sync not finished), so this node's first
// announcement comes on the next block; the manager refuses any list but the base's, so a tick on the tip's list
// would announce nothing that is kept.
BOOST_AUTO_TEST_CASE(the_tick_speaks_from_the_epoch_base_s_list)
{
    BecomeMasternode(); // MN#1 of the lists installed below
    m_node.mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    ConnectActivationBlock(2);
    SyncWithValidationInterfaceQueue();
    const uint256 me{uint256S("1")};
    BOOST_REQUIRE(!dslman->HasResponded(me)); // the base's tick did not run
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    ReaderTipFollower follower{*dmnman};
    RegisterValidationInterface(&follower);
    MineBlock(m_node, CScript{} << OP_TRUE); // position 1 of epoch 10: this node's first tick of the epoch
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(&follower);

    const auto* tip = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
    BOOST_REQUIRE_EQUAL(tip->nHeight, ACTIVATION_HEIGHT + 1);
    // the reader's tip followed the chain, so the tip's list is the next block's, not the base's
    BOOST_REQUIRE(dmnman->GetListAtChainTip().GetBlockHash() == tip->GetBlockHash());
    BOOST_CHECK(dmnman->GetListForBlock(base).GetBlockHash() == base->GetBlockHash());
    BOOST_CHECK_EQUAL(dslman->CurrentEpoch(), 10U);
    BOOST_CHECK(dslman->HasResponded(me)); // announced, and kept, against the base's list
}

// The record of a held announcement is its serialization's hash (dsl::ResponseWireHash): the bytes that read back to
// that announcement. With a valid signature those are the bytes it arrived as. A signature in the other scheme's
// encoding is read, by the reader's fallback, as an invalid one, which serializes as zeros: copies of the bytes such
// an announcement arrived as are read on the ordinary path, and its zero form is recognised unread. Both join the same
// entry, which HoldDSLEarlyResponse's own comparison (masternode and signature) decides for them as well: the vouch
// rule is unchanged, only which bytes skip decoding.
BOOST_AUTO_TEST_CASE(a_held_record_is_the_canonical_form_of_its_announcement)
{
    const auto block = PrepareNextEpochBase();
    other_peer = MakeTestPeer(1);
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->InitializeNode(*other_peer, NODE_NETWORK);
    }
    // An announcement for MN#2 with its signature in the legacy encoding. The reader tries the basic scheme first and
    // falls back to legacy only where the basic reading is not canonical, which depends on the point: draw signatures
    // until one takes the fallback.
    const uint32_t epoch = NEXT_EPOCH_HEIGHT / Params().GetConsensus().nDSLEpochInterval;
    const uint256 mn2 = uint256S("2");
    CDataStream other_scheme{SER_NETWORK, PROTOCOL_VERSION};
    dsl::CPoSeServiceResponse decoded;
    for (int attempt = 0;; ++attempt) {
        BOOST_REQUIRE(attempt < 64);
        CBLSSecretKey key;
        key.MakeNewKey();
        const auto legacy = key.Sign(uint256S(std::to_string(attempt + 100)), /*specificLegacyScheme=*/false)
                                .ToBytes(/*specificLegacyScheme=*/true);
        other_scheme.clear();
        other_scheme << epoch << mn2;
        for (const uint8_t b : legacy) other_scheme << b;
        CDataStream read{other_scheme};
        decoded = dsl::CPoSeServiceResponse{};
        read >> decoded; // the legacy reading is canonical, so this does not throw
        if (!decoded.sig.IsValid()) break;
    }
    BOOST_REQUIRE_EQUAL(other_scheme.size(), dsl::CPoSeServiceResponse::WIRE_SIZE);
    BOOST_REQUIRE(decoded.nEpoch == epoch && decoded.proTxHash == mn2);
    const auto arrived = dsl::PeekResponseWire(MakeUCharSpan(other_scheme));
    BOOST_REQUIRE(arrived);
    BOOST_CHECK(arrived->hash != dsl::ResponseWireHash(decoded)); // not the hash of the bytes it arrived as
    CDataStream zero_form{SER_NETWORK, PROTOCOL_VERSION};
    zero_form << decoded; // the epoch, the masternode and 96 zero bytes
    const auto canonical = dsl::PeekResponseWire(MakeUCharSpan(zero_form));
    BOOST_REQUIRE(canonical);
    BOOST_CHECK(canonical->hash == dsl::ResponseWireHash(decoded)); // but of the bytes that read back to it
    {
        CDataStream read{zero_form};
        dsl::CPoSeServiceResponse again;
        read >> again;
        BOOST_CHECK(again.nEpoch == decoded.nEpoch && again.proTxHash == decoded.proTxHash && again.sig == decoded.sig);
    }
    dsl::SetPerfEnabled(true);
    dsl::TakePerfSnapshot();
    {
        // One block before its base: read and held unverified; the same bytes from another peer are read again and
        // join the same entry, by HoldDSLEarlyResponse's comparison.
        ASSERT_DEBUG_LOG("arrived 1 block(s) before its base block, already held, vouched for by 2 peer(s), peer=");
        SendRaw(other_scheme, *peer);
        SendRaw(other_scheme, *other_peer);
    }
    auto counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 2U);
    BOOST_CHECK_EQUAL(counts.unread, 0U);
    {
        // The zero form reads to the same entry and is recognised unread; its sender already vouches for it.
        ASSERT_DEBUG_LOG("arrived 1 block(s) before its base block, already held, vouched for by 2 peer(s), peer=");
        SendRaw(zero_form, *other_peer);
    }
    counts = TakeResponseCounts();
    BOOST_CHECK_EQUAL(counts.read, 0U);
    BOOST_CHECK_EQUAL(counts.unread, 1U);
    // At its base the entry is refused for its signature, as any invalid one is.
    ConnectNextEpochBase(block);
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK(!dslman->HasResponded(mn2));
}

BOOST_AUTO_TEST_SUITE_END()
