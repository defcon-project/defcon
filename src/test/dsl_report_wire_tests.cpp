// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// What a node does with a POSEREPORT message, through the real PeerManager
// dispatch: which copies are stored and relayed, which are dropped quietly and
// which cost the sender a score. The flood hands a node every report about as
// many times as it has peers, so nearly all of this traffic is a copy of
// something already held. Whatever is done to make copies cheaper has to leave
// every outcome below exactly as it is.

#include <arith_uint256.h>
#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/pose_service_manager.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_sentinels.h>
#include <key.h>
#include <masternode/meta.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
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
#include <array>
#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace {
constexpr int ACTIVATION_HEIGHT{240}; // the base block of epoch 10
constexpr uint32_t EPOCH{10};
constexpr size_t MN_COUNT{12};
constexpr int UNREADABLE_SCORE{10}; // DSL_MSG_MISBEHAVING_UNREADABLE in net_processing.cpp

using dsl::PerfMetric;

// Counter totals over every epoch bucket of the perf window.
struct Tally {
    std::array<uint64_t, static_cast<size_t>(PerfMetric::COUNT)> count{};
    uint64_t operator[](PerfMetric metric) const { return count[static_cast<size_t>(metric)]; }
    void Add(const dsl::PerfSnapshot& snapshot)
    {
        for (const auto& epoch : snapshot.epochs) {
            for (size_t i = 0; i < count.size(); ++i) count[i] += epoch.values[i].count;
        }
    }
};

// Holds the validation interface queue on its scheduler thread, so that what a
// block event would make the store do waits. Always released before the
// fixture goes, failing test or not.
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

struct ReportWireSetup : RegTestingSetup {
    // A connection manager that can hold test nodes, so that a relayed message
    // has somewhere to land.
    ConnmanTestMsg connman{0x1337, 0x1337, *m_node.addrman, *m_node.netgroupman};
    std::unique_ptr<CDeterministicMNManager> dmnman;
    std::unique_ptr<dsl::CPoSeServiceManager> dslman{std::make_unique<dsl::CPoSeServiceManager>()};
    std::unique_ptr<PeerManager> peerman;
    std::vector<CBLSSecretKey> operator_keys; // by masternode index
    std::vector<uint256> protx;               // by masternode index
    std::unique_ptr<CNode> peer_a;
    std::unique_ptr<CNode> peer_b;
    CNode* listener{nullptr}; // owned by connman
    const CBlockIndex* base{nullptr}; // the base block of epoch 10
    Tally tally;
    bool previous_perf{dsl::PerfEnabled()};

    ReportWireSetup() : RegTestingSetup{{"-testactivationheight=dsl@240", "-dip3params=2:2"}}
    {
        dsl::SetPerfEnabled(false);
        dsl::TakePerfSnapshot();
        for (int height = 1; height < ACTIVATION_HEIGHT; ++height) {
            MineBlock(m_node, CScript{} << OP_TRUE);
        }
        SyncWithValidationInterfaceQueue();
        static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).JumpOutOfIbd();
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(false));
        BOOST_REQUIRE(m_node.mn_metaman->LoadCache(false));
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());
        // A reader of its own: the chainstate and quorum objects keep the live
        // deterministic-MN manager, and the synthetic list below is shared with
        // this one through evodb.
        dmnman = std::make_unique<CDeterministicMNManager>(m_node.chainman->ActiveChainstate(), *m_node.evodb);
        peerman = PeerManager::make(Params(), connman, *m_node.addrman, m_node.banman.get(), *m_node.chainman,
                                    *m_node.mempool, *m_node.mn_metaman, *m_node.mn_sync, *m_node.govman,
                                    *m_node.sporkman, nullptr, dmnman, m_node.cj_ctx, m_node.llmq_ctx, dslman, false);
        peerman->SetBestHeight(ACTIVATION_HEIGHT - 1);
        RegisterValidationInterface(peerman.get());

        peer_a = MakeTestPeer(0);
        peer_b = MakeTestPeer(1);
        listener = MakeTestPeer(2).release();
        connman.AddTestNode(*listener);
        {
            LOCK(NetEventsInterface::g_msgproc_mutex);
            peerman->InitializeNode(*peer_a, NODE_NETWORK);
            peerman->InitializeNode(*peer_b, NODE_NETWORK);
            peerman->InitializeNode(*listener, NODE_NETWORK);
        }

        MineBlock(m_node, CScript{} << OP_TRUE);
        base = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE_EQUAL(base->nHeight, ACTIVATION_HEIGHT);
        InstallList(base);
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH);
    }

    ~ReportWireSetup()
    {
        SyncWithValidationInterfaceQueue();
        UnregisterValidationInterface(peerman.get());
        peerman->FinalizeNode(*peer_a);
        peerman->FinalizeNode(*peer_b);
        peerman->FinalizeNode(*listener);
        connman.ClearTestNodes();
        dsl::SetPerfEnabled(previous_perf);
        dsl::TakePerfSnapshot();
    }

    // The same masternodes, with operator keys of our own, as the list at `at`.
    // They are confirmed, because the sentinel assignment only scores confirmed
    // masternodes. The keys are made once; every base block gets its own list.
    void InstallList(const CBlockIndex* at)
    {
        CDeterministicMNList list{at->GetBlockHash(), at->nHeight, MN_COUNT};
        for (size_t i = 0; i < MN_COUNT; ++i) {
            if (operator_keys.size() <= i) {
                CBLSSecretKey key;
                key.MakeNewKey();
                operator_keys.push_back(key);
                protx.push_back(ArithToUint256(arith_uint256{i + 1}));
            }
            auto mn = std::make_shared<CDeterministicMN>(i);
            mn->proTxHash = protx[i];
            mn->collateralOutpoint = COutPoint{mn->proTxHash, 0};
            auto state = std::make_shared<CDeterministicMNState>();
            state->nRegisteredHeight = 1;
            state->UpdateConfirmedHash(mn->proTxHash, at->GetBlockHash());
            CKey owner_key;
            owner_key.MakeNewKey(true);
            state->keyIDOwner = owner_key.GetPubKey().GetID();
            state->pubKeyOperator.Set(operator_keys[i].GetPublicKey(), /*specificLegacyScheme=*/false);
            mn->pdmnState = state;
            list.AddMN(mn);
        }
        m_node.evodb->Write(std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), at->GetBlockHash()), list);
        dmnman->UpdatedBlockTip(at);
        BOOST_REQUIRE_EQUAL(dmnman->GetListForBlock(at).GetAllMNsCount(), MN_COUNT);
    }

    // Mine on to the base block of the next epoch, give it its list, and return
    // it. The tip stays well above the activation height, so DSL messages are
    // still read after that block is taken away again.
    const CBlockIndex* AdvanceToNextEpoch()
    {
        const int next_height = ACTIVATION_HEIGHT + static_cast<int>(Params().GetConsensus().nDSLEpochInterval);
        while (WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()) < next_height) {
            MineBlock(m_node, CScript{} << OP_TRUE);
        }
        const CBlockIndex* next{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
        InstallList(next);
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH + 1);
        return next;
    }

    // Disconnect `block` by a reorg, as InvalidateBlock does, and -- unless the
    // queue is held, and the caller says not to wait for it -- let the
    // validation interface catch up.
    void Disconnect(const CBlockIndex* block, bool wait_for_queue = true)
    {
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, const_cast<CBlockIndex*>(block)));
        if (wait_for_queue) SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_LT(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), block->nHeight);
    }

    uint256 Target() const { return protx.at(0); }

    // The sentinels the epoch whose base block is `at` (by default epoch 10's) assigns to `target`.
    std::vector<uint256> Sentinels(const uint256& target, const CBlockIndex* at = nullptr) const
    {
        const CBlockIndex* block{at != nullptr ? at : base};
        const auto sentinels = dsl::CalcSentinelsForMN(dmnman->GetListForBlock(block), target, block->GetBlockHash(),
                                                       static_cast<size_t>(Params().GetConsensus().nDSLSentinelCount));
        BOOST_REQUIRE_GE(sentinels.size(), 3U);
        return sentinels;
    }

    // A masternode of the list that the epoch did not assign to `target`.
    uint256 Outsider(const uint256& target) const
    {
        const auto sentinels = Sentinels(target);
        for (const auto& hash : protx) {
            if (hash != target && std::find(sentinels.begin(), sentinels.end(), hash) == sentinels.end()) return hash;
        }
        BOOST_FAIL("every masternode is a sentinel of the target");
        return {};
    }

    const CBLSSecretKey& KeyOf(const uint256& id) const
    {
        const auto it = std::find(protx.begin(), protx.end(), id);
        BOOST_REQUIRE(it != protx.end());
        return operator_keys.at(static_cast<size_t>(it - protx.begin()));
    }

    // A report signed by `sentinel` for the epoch whose base block is `at` (by default epoch 10's).
    dsl::CPoSeServiceReport Report(const uint256& target, const uint256& sentinel,
                                   dsl::ServiceStatus status = dsl::ServiceStatus::ONLINE,
                                   const CBlockIndex* at = nullptr) const
    {
        const CBlockIndex* block{at != nullptr ? at : base};
        dsl::CPoSeServiceReport report;
        report.nEpoch = static_cast<uint32_t>(block->nHeight / Params().GetConsensus().nDSLEpochInterval);
        report.targetProTxHash = target;
        report.sentinelProTxHash = sentinel;
        report.status = static_cast<uint8_t>(status);
        report.Sign(KeyOf(sentinel), block->GetBlockHash());
        return report;
    }

    static std::vector<unsigned char> Wire(const dsl::CPoSeServiceReport& report)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << report;
        return {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()};
    }

    // `wire` with its last 96 bytes -- the signature -- replaced.
    static std::vector<unsigned char> WithSignatureBytes(std::vector<unsigned char> wire, const std::array<uint8_t, 96>& sig)
    {
        BOOST_REQUIRE_GE(wire.size(), sig.size());
        std::copy(sig.begin(), sig.end(), wire.end() - static_cast<std::ptrdiff_t>(sig.size()));
        return wire;
    }

    void Send(const std::vector<unsigned char>& bytes, CNode& from)
    {
        CDataStream payload{MakeUCharSpan(bytes), SER_NETWORK, PROTOCOL_VERSION};
        LOCK(NetEventsInterface::g_msgproc_mutex);
        SendMessage(*peerman, from, NetMsgType::POSEREPORT, std::move(payload));
    }
    void Send(const dsl::CPoSeServiceReport& report, CNode& from) { Send(Wire(report), from); }

    // Start counting: everything from here on lands in `tally`.
    void Observe()
    {
        dsl::TakePerfSnapshot();
        tally = Tally{};
        dsl::SetPerfEnabled(true);
    }
    const Tally& Poll()
    {
        tally.Add(dsl::TakePerfSnapshot());
        return tally;
    }

    // POSEREPORT messages that reached the listener, that is, were relayed.
    size_t Relayed() const
    {
        LOCK(listener->cs_vSend);
        return static_cast<size_t>(std::count_if(listener->vSendMsg.begin(), listener->vSendMsg.end(),
                                                 [](const CSerializedNetMsg& msg) { return msg.m_type == NetMsgType::POSEREPORT; }));
    }

    int Score(const CNode& peer) const { return MisbehaviorScore(*peerman, peer); }
    size_t Held() const { return dslman->Store().Size(); }

    // The held report for a key, if any.
    std::optional<dsl::CPoSeServiceReport> HeldReport(const uint256& target, const uint256& sentinel) const
    {
        for (const auto& report : dslman->Store().GetReportsForEpoch(EPOCH)) {
            if (report.targetProTxHash == target && report.sentinelProTxHash == sentinel) return report;
        }
        return std::nullopt;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dsl_report_wire_tests, ReportWireSetup)

// The case the whole flood is made of. The first copy is verified, stored and
// forwarded; every later copy, whichever peer it comes from, is dropped
// without a word -- not stored again, not forwarded, not scored.
BOOST_AUTO_TEST_CASE(exact_copies_are_neither_stored_relayed_nor_scored)
{
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target).at(0)};
    const auto report = Report(target, sentinel);
    Observe();

    Send(report, *peer_a);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, target, sentinel));
    BOOST_CHECK_EQUAL(Relayed(), 1U);

    for (int i = 0; i < 3; ++i) Send(report, *peer_a);
    for (int i = 0; i < 2; ++i) Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
    BOOST_CHECK_EQUAL(Score(*peer_a), 0);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 6U);
    BOOST_CHECK_EQUAL(c[PerfMetric::ACCEPTED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE], 5U);
    BOOST_CHECK_EQUAL(c[PerfMetric::BUDGET_REFUSED] + c[PerfMetric::DECODE_REFUSED] + c[PerfMetric::BASE_REFUSED] +
                      c[PerfMetric::REFUSED], 0U);
    // every message the budget let through ended in exactly one of the three outcomes
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], c[PerfMetric::ACCEPTED] + c[PerfMetric::DUPLICATE]);
}

// A copy is recognised by its key and not by its signature: the sentinel's first
// report for a target in an epoch is the one that counts, whatever follows it
// under the same key -- including a second, validly signed, contradicting one.
BOOST_AUTO_TEST_CASE(the_first_report_under_a_key_wins)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto first = Report(target, sentinels.at(0), dsl::ServiceStatus::ONLINE);
    const auto other = Report(target, sentinels.at(1), dsl::ServiceStatus::ONLINE);
    Send(first, *peer_a);
    Send(other, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 2U);
    BOOST_REQUIRE_EQUAL(Relayed(), 2U);
    Observe();

    // the first sentinel's key carrying the second one's signature: a real curve
    // point, so it reads, and it is the duplicate it looks like
    auto borrowed = first;
    borrowed.sig = other.sig;
    Send(borrowed, *peer_b);

    // the same key and signature under another status
    auto flipped = first;
    flipped.status = static_cast<uint8_t>(dsl::ServiceStatus::MISSED);
    Send(flipped, *peer_b);

    // the same key, honestly signed for the other status
    Send(Report(target, sentinels.at(0), dsl::ServiceStatus::MISSED), *peer_b);

    BOOST_CHECK_EQUAL(Held(), 2U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
    const auto held = HeldReport(target, sentinels.at(0));
    BOOST_REQUIRE(held.has_value());
    BOOST_CHECK_EQUAL(held->status, static_cast<uint8_t>(dsl::ServiceStatus::ONLINE));
    BOOST_CHECK(held->sig.ToBytes(false) == first.sig.ToBytes(false));

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 3U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE], 3U);
    BOOST_CHECK_EQUAL(c[PerfMetric::ACCEPTED] + c[PerfMetric::REFUSED] + c[PerfMetric::DECODE_REFUSED], 0U);
}

// What cannot be read is scored, and it is scored whether or not the key is
// held: the duplicate check comes after the read, so a held key does not make
// garbage free.
BOOST_AUTO_TEST_CASE(an_unreadable_signature_is_scored_under_a_held_key_as_under_any_other)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto held = Report(target, sentinels.at(0));
    Send(held, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    // 0xFF.. sets the compression, infinity and sign flags together with a
    // non-zero body: no encoder produces it and no point decodes from it
    const std::array<uint8_t, 96> garbage = [] { std::array<uint8_t, 96> a; a.fill(0xFF); return a; }();
    Send(WithSignatureBytes(Wire(held), garbage), *peer_b);                      // held key
    BOOST_CHECK_EQUAL(Score(*peer_b), UNREADABLE_SCORE);
    Send(WithSignatureBytes(Wire(Report(target, sentinels.at(1))), garbage), *peer_b); // unheld key
    BOOST_CHECK_EQUAL(Score(*peer_b), 2 * UNREADABLE_SCORE);

    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
    BOOST_CHECK_EQUAL(Score(*peer_a), 0);
    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REFUSED], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE] + c[PerfMetric::ACCEPTED] + c[PerfMetric::REFUSED], 0U);
}

// Two encodings that read without an error and without a point: an all-zero
// signature, and the legacy serialization of a genuine one (the reader tries
// the other scheme before it gives up, and then treats the object as invalid).
// Neither is scored; under a held key each is a duplicate, under any other it
// fails verification.
BOOST_AUTO_TEST_CASE(zero_and_legacy_encoded_signatures_read_without_a_score)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto held = Report(target, sentinels.at(0));
    const auto unheld = Report(target, sentinels.at(1));
    Send(held, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    const std::array<uint8_t, 96> zero{};
    const auto legacy_of = [](const dsl::CPoSeServiceReport& report) { return report.sig.ToBytes(/*specificLegacyScheme=*/true); };

    Send(WithSignatureBytes(Wire(held), zero), *peer_b);
    Send(WithSignatureBytes(Wire(held), legacy_of(held)), *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE], 2U);

    Send(WithSignatureBytes(Wire(unheld), zero), *peer_b);
    Send(WithSignatureBytes(Wire(unheld), legacy_of(unheld)), *peer_b);
    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::REFUSED], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REFUSED], 0U);
    BOOST_CHECK_EQUAL(c[PerfMetric::ACCEPTED], 0U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
}

// The reader takes what it needs from the front of the payload and ignores the
// rest, so a longer payload is the same report; a shorter one is unreadable.
BOOST_AUTO_TEST_CASE(payload_length_is_read_as_the_reader_reads_it)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto held = Report(target, sentinels.at(0));
    const auto fresh = Report(target, sentinels.at(1));
    Send(held, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    auto longer_copy = Wire(held);
    longer_copy.push_back(0x00);
    Send(longer_copy, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE], 1U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);

    // trailing bytes do not stop a new report: it is stored and forwarded
    auto longer_fresh = Wire(fresh);
    longer_fresh.insert(longer_fresh.end(), {0xDE, 0xAD});
    Send(longer_fresh, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::ACCEPTED], 1U);
    BOOST_CHECK_EQUAL(Held(), 2U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);

    auto shorter = Wire(held);
    shorter.pop_back();
    Send(shorter, *peer_b);
    BOOST_CHECK_EQUAL(Score(*peer_b), UNREADABLE_SCORE);
    Send(std::vector<unsigned char>{}, *peer_b);
    BOOST_CHECK_EQUAL(Score(*peer_b), 2 * UNREADABLE_SCORE);
    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REFUSED], 2U);
    BOOST_CHECK_EQUAL(Held(), 2U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
}

// A signature the node has already seen is no passport: borrowed under another
// sentinel, another target, another epoch or by a masternode the epoch never
// assigned, it is a new report, and it fails.
BOOST_AUTO_TEST_CASE(a_known_signature_under_another_key_is_a_new_report_and_fails)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto held = Report(target, sentinels.at(0));
    Send(held, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    const auto borrow = [&](auto mutate) {
        auto report = held;
        mutate(report);
        Send(report, *peer_b);
    };
    borrow([&](auto& r) { r.sentinelProTxHash = sentinels.at(1); });  // another assigned sentinel
    borrow([&](auto& r) { r.sentinelProTxHash = Outsider(target); }); // a masternode the epoch did not assign
    borrow([&](auto& r) { r.targetProTxHash = protx.at(1); });        // another target
    borrow([&](auto& r) { r.nEpoch = EPOCH - 1; });                   // another epoch of the window

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 4U);
    BOOST_CHECK_EQUAL(c[PerfMetric::REFUSED], 4U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE] + c[PerfMetric::ACCEPTED] + c[PerfMetric::DECODE_REFUSED], 0U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
}

// A held report is only a duplicate while the store would still look for it.
// Outside its epoch window the store refuses (and says so in a different
// counter), and what it has dropped is new again when it comes back.
BOOST_AUTO_TEST_CASE(a_copy_follows_the_epoch_window_of_the_store)
{
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target).at(0)};
    const auto report = Report(target, sentinel);
    Send(report, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    BOOST_REQUIRE_EQUAL(Relayed(), 1U);
    Observe();

    // the store's epoch behind the report's: refused as from the future, though held
    dslman->Store().SetCurrentEpoch(EPOCH - 1);
    BOOST_CHECK_EQUAL(Held(), 1U);
    Send(report, *peer_b);
    {
        const auto& c = Poll();
        BOOST_CHECK_EQUAL(c[PerfMetric::REFUSED], 1U);
        BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE], 0U);
    }

    // back on its epoch it is a duplicate again
    dslman->Store().SetCurrentEpoch(EPOCH);
    Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE], 1U);

    // the store far enough ahead that the report's epoch has aged out: dropped
    // from the store, and refused as aged out
    dslman->Store().SetCurrentEpoch(EPOCH + 8);
    BOOST_CHECK_EQUAL(Held(), 0U);
    Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::REFUSED], 2U);

    // once the epoch is current again, the same bytes are a new report: verified,
    // stored and forwarded a second time
    dslman->Store().SetCurrentEpoch(EPOCH);
    Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::ACCEPTED], 1U);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
}

// What a reorg across the epoch's base does to the store is DropEpoch. The
// copies that follow are verified again, against whatever base is now active.
BOOST_AUTO_TEST_CASE(a_dropped_epoch_takes_the_same_bytes_again)
{
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target).at(0)};
    const auto report = Report(target, sentinel);
    Send(report, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    dslman->Store().DropEpoch(EPOCH);
    BOOST_CHECK_EQUAL(Held(), 0U);
    Send(report, *peer_b);
    Send(report, *peer_b);

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::ACCEPTED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE], 1U);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
}

// The budget is charged before any of this, so copies spend it like anything
// else and are cut off the same way. Another peer's budget is its own.
BOOST_AUTO_TEST_CASE(copies_spend_the_per_peer_budget)
{
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target).at(0)};
    const auto report = Report(target, sentinel);
    Observe();

    Send(report, *peer_a);
    // the budget of a connection this node opened is about one epoch's honest
    // traffic (514 messages here); far fewer than this are needed to run it dry
    size_t sent{1};
    while (Poll()[PerfMetric::BUDGET_REFUSED] == 0 && sent < 2000) {
        Send(report, *peer_a);
        ++sent;
    }
    BOOST_REQUIRE_GT(tally[PerfMetric::BUDGET_REFUSED], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::ACCEPTED], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::WIRE_REPORT], tally[PerfMetric::ACCEPTED] + tally[PerfMetric::DUPLICATE] +
                                                          tally[PerfMetric::BUDGET_REFUSED]);
    BOOST_CHECK_EQUAL(Score(*peer_a), 0);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 1U);

    // one more from the same peer is refused again, one from the other is not
    const uint64_t duplicates{tally[PerfMetric::DUPLICATE]};
    const uint64_t refused{tally[PerfMetric::BUDGET_REFUSED]};
    Send(report, *peer_a);
    Send(report, *peer_b);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::BUDGET_REFUSED], refused + 1);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE], duplicates + 1);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
}

// The epoch's base block is looked up before the store is asked anything, so a
// node that no longer has the base on its chain refuses the message for that,
// held report or not.
BOOST_AUTO_TEST_CASE(a_missing_epoch_base_is_refused_before_the_store_is_asked)
{
    const CBlockIndex* next{AdvanceToNextEpoch()};
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target, next).at(0)};
    const auto report = Report(target, sentinel, dsl::ServiceStatus::ONLINE, next);
    BOOST_REQUIRE_EQUAL(report.nEpoch, EPOCH + 1);
    Send(report, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);

    // an epoch whose base the chain has not reached yet
    auto ahead = report;
    ahead.nEpoch = EPOCH + 3;

    // and the epoch's own base, disconnected by a reorg: the store still holds
    // the report, and the chain is still past the activation height
    Disconnect(next);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    Send(report, *peer_b);
    Send(ahead, *peer_b);
    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::BASE_REFUSED], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE] + c[PerfMetric::ACCEPTED] + c[PerfMetric::REFUSED], 0U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
}

// The base is looked up on the active chain, which a reorg changes at once; the
// store follows through the validation queue, which can lag. For that while the
// store still holds, and still has the epoch current for, a report whose base
// the chain has lost -- and a copy of it is refused for the missing base, as
// ever, and not as a duplicate.
BOOST_AUTO_TEST_CASE(a_base_lost_before_the_store_hears_of_it_still_refuses_for_the_base)
{
    const CBlockIndex* next{AdvanceToNextEpoch()};
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target, next).at(0)};
    const auto report = Report(target, sentinel, dsl::ServiceStatus::ONLINE, next);
    Send(report, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);

    BlockValidationQueue queue;
    Disconnect(next, /*wait_for_queue=*/false);
    BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH + 1); // the store has not been told
    Observe();

    Send(report, *peer_b);
    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::BASE_REFUSED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE] + c[PerfMetric::ACCEPTED] + c[PerfMetric::REFUSED], 0U);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
    queue.Release();
}

// ---------------------------------------------------------------------------
// The cases above pin what is done with a message and hold for any way of
// doing it. The ones below pin how much reading it takes: an exact copy of a
// held report is dropped from its bytes, and everything else is read.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(exact_copies_of_a_held_report_are_dropped_unread)
{
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target).at(0)};
    const auto report = Report(target, sentinel);
    Observe();

    Send(report, *peer_a); // the first copy is read, verified and stored
    for (int i = 0; i < 4; ++i) Send(report, *peer_b);

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 5U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REPORT], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE_UNDECODED], 4U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE], 4U);   // still duplicates, as before
    BOOST_CHECK_EQUAL(c[PerfMetric::ACCEPTED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::STORE_WAIT], 1U);  // the store was asked once, by the first copy
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 1U);
    BOOST_CHECK_EQUAL(Score(*peer_a), 0);
    BOOST_CHECK_EQUAL(Score(*peer_b), 0);
}

// One byte different, anywhere, and the message is read like any other.
BOOST_AUTO_TEST_CASE(a_message_that_is_not_an_exact_copy_is_read)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto held = Report(target, sentinels.at(0));
    const auto other = Report(target, sentinels.at(1));
    Send(held, *peer_a);
    Send(other, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 2U);
    Observe();

    const std::vector<unsigned char> exact{Wire(held)};
    size_t sent{0};
    const auto send_variant = [&](std::vector<unsigned char> bytes) {
        Send(bytes, *peer_b);
        ++sent;
    };
    for (size_t offset : {size_t{0}, size_t{4}, size_t{36}, size_t{68}, size_t{69}, exact.size() - 1}) {
        // each field of the key, the status, and the first and last byte of the signature
        auto bytes = exact;
        bytes.at(offset) ^= 0x01;
        send_variant(std::move(bytes));
    }
    send_variant(WithSignatureBytes(exact, std::array<uint8_t, 96>{}));                        // all-zero signature
    send_variant(WithSignatureBytes(exact, held.sig.ToBytes(/*specificLegacyScheme=*/true)));  // legacy encoding
    send_variant(WithSignatureBytes(exact, other.sig.ToBytes(false)));                         // another real signature
    auto longer = exact;
    longer.push_back(0x00);
    send_variant(std::move(longer));
    auto shorter = exact;
    shorter.pop_back();
    send_variant(std::move(shorter));
    send_variant({});

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], sent);
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REPORT], sent);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE_UNDECODED], 0U);
    // and the exact copy is still recognised afterwards
    Send(held, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 1U);
    BOOST_CHECK_EQUAL(Held(), 2U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
}

// The store's own answer decides, so everything that changes it changes the
// shortcut with it: a copy is read whenever the store would not take it as a
// duplicate, and is dropped unread again once it would.
BOOST_AUTO_TEST_CASE(the_shortcut_follows_the_store_and_not_a_memory_of_its_own)
{
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target).at(0)};
    const auto report = Report(target, sentinel);
    Send(report, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);
    Observe();

    Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 1U);

    dslman->Store().SetCurrentEpoch(EPOCH - 1); // held, but the store would refuse it as from the future
    Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DECODE_REPORT], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::REFUSED], 1U);

    dslman->Store().SetCurrentEpoch(EPOCH);
    Send(report, *peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 2U);

    dslman->Store().DropEpoch(EPOCH); // what a reorg of the base does
    Send(report, *peer_b);            // read, verified, stored again
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 2U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DECODE_REPORT], 2U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::ACCEPTED], 1U);
    Send(report, *peer_a);            // and a copy of the new one is dropped unread
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 3U);
    BOOST_CHECK_EQUAL(Held(), 1U);
    BOOST_CHECK_EQUAL(Relayed(), 2U);
}

// Without the epoch's base block on the chain the message is refused for that
// before anything about the report is asked -- and, as ever, only after it has
// been read. The store still holds the report and still has its epoch current
// (the queue is held back), so only the base keeps the shortcut from taking it.
BOOST_AUTO_TEST_CASE(the_shortcut_waits_for_the_epoch_base)
{
    const CBlockIndex* next{AdvanceToNextEpoch()};
    const uint256 target{Target()};
    const uint256 sentinel{Sentinels(target, next).at(0)};
    const auto report = Report(target, sentinel, dsl::ServiceStatus::ONLINE, next);
    Send(report, *peer_a);
    BOOST_REQUIRE_EQUAL(Held(), 1U);

    BlockValidationQueue queue;
    Disconnect(next, /*wait_for_queue=*/false);
    BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH + 1);
    Observe();

    Send(report, *peer_b);
    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::BASE_REFUSED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REPORT], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE_UNDECODED] + c[PerfMetric::DUPLICATE], 0U);
    queue.Release();
}

// What the counters add up to, over a mix of everything a node is sent. The
// shortcut leaves the old identity as it was and adds one of its own.
BOOST_AUTO_TEST_CASE(counters_account_for_every_message)
{
    const uint256 target{Target()};
    const auto sentinels = Sentinels(target);
    const auto first = Report(target, sentinels.at(0));
    const auto second = Report(target, sentinels.at(1));
    std::array<uint8_t, 96> garbage;
    garbage.fill(0xFF);
    Observe();

    Send(first, *peer_a);                                       // accepted
    Send(first, *peer_b);                                       // exact copy
    Send(first, *peer_b);                                       // exact copy
    Send(second, *peer_a);                                      // accepted
    auto borrowed = first;
    borrowed.sig = second.sig;
    Send(borrowed, *peer_b);                                    // read, a duplicate
    auto stolen = first;
    stolen.sentinelProTxHash = sentinels.at(2);
    Send(stolen, *peer_b);                                      // read, refused
    Send(WithSignatureBytes(Wire(first), garbage), *peer_b);    // unreadable
    auto ahead = first;
    ahead.nEpoch = EPOCH + 3;
    Send(ahead, *peer_b);                                       // no such base

    const auto& c = Poll();
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT], 8U);
    BOOST_CHECK_EQUAL(c[PerfMetric::WIRE_REPORT],
                      c[PerfMetric::BUDGET_REFUSED] + c[PerfMetric::DECODE_REFUSED] + c[PerfMetric::BASE_REFUSED] +
                      c[PerfMetric::ACCEPTED] + c[PerfMetric::DUPLICATE] + c[PerfMetric::REFUSED]);
    BOOST_CHECK_EQUAL(c[PerfMetric::ACCEPTED], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE], 3U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DUPLICATE_UNDECODED], 2U);
    BOOST_CHECK_EQUAL(c[PerfMetric::REFUSED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REFUSED], 1U);
    BOOST_CHECK_EQUAL(c[PerfMetric::BASE_REFUSED], 1U);
    // every message the budget let through was read, except the exact copies
    BOOST_CHECK_EQUAL(c[PerfMetric::DECODE_REPORT],
                      c[PerfMetric::WIRE_REPORT] - c[PerfMetric::BUDGET_REFUSED] - c[PerfMetric::DUPLICATE_UNDECODED]);
    // and the store was asked for each of those that got that far
    BOOST_CHECK_EQUAL(c[PerfMetric::STORE_WAIT],
                      c[PerfMetric::ACCEPTED] + c[PerfMetric::DUPLICATE] - c[PerfMetric::DUPLICATE_UNDECODED] + c[PerfMetric::REFUSED]);
}

BOOST_AUTO_TEST_SUITE_END()
