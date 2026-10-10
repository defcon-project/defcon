// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/pose_service_manager.h>
#include <evo/pose_service_metrics.h>
#include <key.h>
#include <masternode/meta.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>
#include <future>
#include <memory>

namespace {
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
                                   *m_node.govman, *m_node.sporkman, nullptr, dmnman,
                                   m_node.cj_ctx, m_node.llmq_ctx, dslman, false);
    }

    void InstallList(size_t count)
    {
        CDeterministicMNList list{base->GetBlockHash(), base->nHeight, count};
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
        // This reader has not cached the newly connected block. No component's
        // existing manager is replaced or freed, and mining uses the original.
        // The synthetic snapshot is shared through evodb with those managers.
        m_node.evodb->Write(std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), base->GetBlockHash()), list);
        dmnman->UpdatedBlockTip(base); // production CDSNotificationInterface's synchronous update
        BOOST_REQUIRE_EQUAL(dmnman->GetListForBlock(base).GetAllMNsCount(), count);
    }

    void ConnectActivationBlock(size_t count = 1, const CScript& script = CScript{} << OP_TRUE)
    {
        // ActivateBestChain would wait for the blocked queue above ten callbacks.
        BOOST_REQUIRE_LE(GetMainSignals().CallbacksPending(), 10);
        MineBlock(m_node, script);
        base = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE_EQUAL(base->nHeight, ACTIVATION_HEIGHT);
        InstallList(count);
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
        auto block = PrepareBlock(m_node, CScript{} << OP_TRUE);
        while (!CheckProofOfWork(block->GetHash(), block->nBits, Params().GetConsensus())) {
            ++block->nNonce;
            BOOST_REQUIRE(block->nNonce != 0);
        }
        return block;
    }

    void ConnectNextEpochBase(const std::shared_ptr<CBlock>& block)
    {
        BOOST_REQUIRE_LE(GetMainSignals().CallbacksPending(), 10);
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(Params(), block, true, nullptr));
        base = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE_EQUAL(base->nHeight, NEXT_EPOCH_HEIGHT);
        BOOST_REQUIRE(base->GetBlockHash() == block->GetHash());
        InstallList(2);
    }

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

BOOST_AUTO_TEST_SUITE_END()
