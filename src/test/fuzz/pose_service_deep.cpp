// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! The four PoSe service messages past the parser.
//!
//! The process_message_pose* targets reach deserialization and stop there: their regtest node has no masternode
//! list, is no masternode and has no epoch, so every input is refused at the first lookup. Here the node has an
//! epoch, a masternode list at that epoch's base and at the one before (synthetic, as in dsl_activation_tests,
//! with operator keys the harness holds), is past the blockchain stage of masternode sync, and is one of those
//! masternodes itself. The service starts each input in the current epoch, or still in the previous one, as on a
//! node whose epoch tick is still queued behind the block that opened the epoch. Each input is a short sequence
//! of messages built from the fuzzer's choices -- an epoch near the current one, a listed or unknown proTxHash, a
//! sentinel the epoch assigned to its target or not, a signature that is valid, made with another operator key,
//! made over another base, or none -- serialized, some bytes optionally changed, and handed to the real message
//! dispatch (PeerManager::ProcessMessage) from one peer. So a response and a report reach the BLS signature check
//! and past it (acceptance, duplicate, relay), a response also the holds for one that arrives early, and a
//! challenge reaches the re-announcement. A signed service
//! commitment is checked against an LLMQ quorum that a fuzz fixture cannot form: its target reaches the quorum
//! selection, through every check before it.
//!
//! Replayable: an input sends the same bytes and meets the same state in any process and after any other input.
//! The fixture is deterministic -- fixed BLS and owner keys, a chain mined from the genesis time, a fixed mock time
//! -- and the PeerManager, the service manager and the peer (always id 0) are new for every input, so nothing one
//! input leaves behind (a held early response, a spent budget, an outbound grant) reaches the next. What does
//! outlive an input is the static report signature cache in CPoSeServiceReport::VerifySig: it holds only
//! signatures that verified, keyed by the signed hash, the key and the signature, so a hit returns what the
//! verification would have -- only the BLS call is skipped.

#include <bls/bls.h>
#include <chain.h>
#include <hash.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/pose_service.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_manager.h>
#include <evo/pose_service_sentinels.h>
#include <llmq/options.h>
#include <masternode/meta.h>
#include <masternode/node.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <protocol.h>
#include <pubkey.h>
#include <script/script.h>
#include <streams.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t MN_COUNT{16};
//! Messages one input may send, from one peer.
constexpr int MAX_MESSAGES{8};

struct PoseDeepFixture {
    const TestingSetup* setup{nullptr};
    std::vector<CBLSSecretKey> keys;
    std::vector<uint256> protx;
    const CBlockIndex* base{nullptr};      //!< the current epoch's base, the chain tip
    const CBlockIndex* prev_base{nullptr}; //!< the previous epoch's base
    uint256 confirmed;                     //!< the block every masternode's collateral was confirmed at
    uint32_t epoch{0};
    int interval{0};
    //! (sentinel index, target index) pairs each epoch assigned, by base: index 0 the current, 1 the previous
    std::vector<std::pair<size_t, size_t>> assigned[2];
    std::unique_ptr<CDeterministicMNManager> dmnman;
    std::unique_ptr<dsl::CPoSeServiceManager> dslman;
    std::unique_ptr<CActiveMasternodeManager> activeman;
};

PoseDeepFixture* g_fx{nullptr};

void InstallList(const TestingSetup& setup, PoseDeepFixture& fx, const CBlockIndex* at)
{
    CDeterministicMNList list{at->GetBlockHash(), at->nHeight, static_cast<uint32_t>(MN_COUNT)};
    for (size_t i = 0; i < MN_COUNT; ++i) {
        auto mn = std::make_shared<CDeterministicMN>(i);
        mn->proTxHash = fx.protx[i];
        mn->collateralOutpoint = COutPoint{mn->proTxHash, 0};
        auto state = std::make_shared<CDeterministicMNState>();
        state->nRegisteredHeight = 1;
        // Confirmed, or the list's scores leave it out and it is no one's sentinel (as in dsl_report_wire_tests).
        state->UpdateConfirmedHash(mn->proTxHash, fx.confirmed);
        state->keyIDOwner = CKeyID{Hash160(std::vector<unsigned char>{mn->proTxHash.begin(), mn->proTxHash.end()})};
        state->pubKeyOperator.Set(fx.keys[i].GetPublicKey(), /*specificLegacyScheme=*/false);
        mn->pdmnState = state;
        list.AddMN(mn);
    }
    // A snapshot at the base, read by the fixture's own manager (as dsl_activation_tests does): the node's
    // components keep their own managers and are not touched.
    setup.m_node.evodb->Write(std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), at->GetBlockHash()), list);
}

void initialize_pose_service_deep()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(
        /*chain_name=*/CBaseChainParams::REGTEST,
        // DIP3 from the start, as in dsl_activation_tests: before it every masternode list is empty.
        /*extra_args=*/{"-testactivationheight=dsl@0", "-dip3params=2:2"});
    // Never destroyed: its BLS keys go back to the secure allocator, a static of bls.cpp created after this fixture
    // and so destroyed before it at exit -- a use after free under ASan. Reachable through g_fx, so no leak report.
    static PoseDeepFixture& fx{*new PoseDeepFixture};
    fx.setup = testing_setup.get();
    const auto& node = testing_setup->m_node;
    Assert(node.mn_metaman->LoadCache(false));
    Assert(node.netfulfilledman->LoadCache(false));

    const Consensus::Params& consensus = Params().GetConsensus();
    fx.interval = consensus.nDSLEpochInterval;
    Assert(fx.interval > 0);
    // Two epoch bases on the chain, the tip being the second: the current epoch and the one before both have one.
    for (int height = 1; height <= 2 * fx.interval; ++height) {
        MineBlock(node, CScript() << OP_TRUE);
    }
    SyncWithValidationInterfaceQueue();
    {
        LOCK(cs_main);
        fx.base = node.chainman->ActiveChain().Tip();
        fx.prev_base = node.chainman->ActiveChain()[fx.interval];
        fx.confirmed = node.chainman->ActiveChain()[1]->GetBlockHash();
    }
    Assert(fx.base->nHeight == 2 * fx.interval);
    // The blocks' times come from the genesis time (median time past + 1), not the clock; the clock the messages
    // are handled at is fixed a minute past the tip, which also takes the node out of initial block download.
    SetMockTime(fx.base->GetBlockTime() + 60);
    dsl::SetPerfEnabled(false);
    // Past the blockchain stage, as a synced node is (dsl_report_wire_tests does the same).
    node.mn_sync->SwitchToNextAsset();
    Assert(node.mn_sync->IsBlockchainSynced());
    fx.epoch = static_cast<uint32_t>(fx.base->nHeight / fx.interval);

    for (size_t i = 0; i < MN_COUNT; ++i) {
        // Fixed keys: a hash of the index, its top byte replaced so that it is below the group order.
        CHashWriter seed_writer{SER_GETHASH, 0};
        seed_writer << std::string{"pose_service_deep operator key"} << static_cast<uint64_t>(i);
        const uint256 seed{seed_writer.GetHash()};
        std::array<unsigned char, 32> bytes;
        std::copy(seed.begin(), seed.end(), bytes.begin());
        bytes[0] = static_cast<unsigned char>(0x10 + i);
        CBLSSecretKey key{bytes};
        Assert(key.IsValid());
        fx.keys.push_back(key);
        fx.protx.push_back(uint256S(strprintf("%064x", i + 1)));
    }
    InstallList(*testing_setup, fx, fx.prev_base);
    InstallList(*testing_setup, fx, fx.base);
    fx.dmnman = std::make_unique<CDeterministicMNManager>(node.chainman->ActiveChainstate(), *node.evodb);
    fx.dmnman->UpdatedBlockTip(fx.base);
    Assert(fx.dmnman->GetListForBlock(fx.base).GetAllMNsCount() == MN_COUNT);
    Assert(fx.dmnman->GetListForBlock(fx.prev_base).GetAllMNsCount() == MN_COUNT);

    for (int which = 0; which < 2; ++which) {
        const CBlockIndex* at = which == 0 ? fx.base : fx.prev_base;
        const auto list = fx.dmnman->GetListForBlock(at);
        for (size_t s = 0; s < MN_COUNT; ++s) {
            for (const uint256& target : dsl::GetProbeTargetsForSentinel(list, fx.protx[s], at->GetBlockHash(),
                                                                         static_cast<size_t>(consensus.nDSLSentinelCount))) {
                for (size_t t = 0; t < MN_COUNT; ++t) {
                    if (fx.protx[t] == target) fx.assigned[which].emplace_back(s, t);
                }
            }
        }
        Assert(!fx.assigned[which].empty());
    }

    // This node is masternode 0; resolved from its operator key, as a node is whose manager withholds the identity.
    fx.activeman = std::make_unique<CActiveMasternodeManager>(fx.keys[0], *node.connman, fx.dmnman);
    g_fx = &fx;
}

const CBlockIndex* BaseOf(uint32_t epoch)
{
    if (epoch == g_fx->epoch) return g_fx->base;
    // not epoch + 1: the fuzzer's epoch can be UINT32_MAX, and the integer sanitizer stops on the wrap
    if (epoch == g_fx->epoch - 1) return g_fx->prev_base;
    return nullptr;
}

uint32_t PickEpoch(FuzzedDataProvider& fdp)
{
    switch (fdp.ConsumeIntegralInRange<int>(0, 4)) {
    case 0: return g_fx->epoch;
    case 1: return g_fx->epoch - 1;
    case 2: return g_fx->epoch + 1;
    case 3: return g_fx->epoch + fdp.ConsumeIntegralInRange<uint32_t>(2, 64);
    default: return fdp.ConsumeIntegral<uint32_t>();
    }
}

size_t PickIndex(FuzzedDataProvider& fdp) { return fdp.ConsumeIntegralInRange<size_t>(0, MN_COUNT - 1); }

uint256 PickProTx(FuzzedDataProvider& fdp)
{
    return fdp.ConsumeBool() ? g_fx->protx[PickIndex(fdp)] : ConsumeUInt256(fdp);
}

//! The base hash a signer uses: the epoch's own, the other epoch's, or any.
uint256 PickSignBase(FuzzedDataProvider& fdp, uint32_t epoch)
{
    switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
    case 0: {
        const CBlockIndex* base = BaseOf(epoch);
        return base != nullptr ? base->GetBlockHash() : g_fx->base->GetBlockHash();
    }
    case 1: return (BaseOf(epoch) == g_fx->base ? g_fx->prev_base : g_fx->base)->GetBlockHash();
    default: return ConsumeUInt256(fdp);
    }
}

//! A signer: masternode `own`'s key, another listed one's, or none (the signature left empty).
const CBLSSecretKey* PickKey(FuzzedDataProvider& fdp, size_t own)
{
    switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
    case 0: return &g_fx->keys[own];
    case 1: return &g_fx->keys[PickIndex(fdp)];
    default: return nullptr;
    }
}

size_t IndexOf(const uint256& protx)
{
    for (size_t i = 0; i < MN_COUNT; ++i) {
        if (g_fx->protx[i] == protx) return i;
    }
    return 0;
}

//! Serialize `msg`, change up to a few of its bytes at the fuzzer's choice, and hand it to the node.
//! The PeerManager of the input being run.
PeerManager* g_peerman{nullptr};

template <typename T>
void Deliver(FuzzedDataProvider& fdp, CNode& peer, const std::string& msg_type, const T& msg)
    EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    CDataStream payload{SER_NETWORK, PROTOCOL_VERSION};
    payload << msg;
    for (int edits = fdp.ConsumeIntegralInRange<int>(0, 3); edits > 0 && !payload.empty(); --edits) {
        const size_t at = fdp.ConsumeIntegralInRange<size_t>(0, payload.size() - 1);
        payload[at] = static_cast<CDataStream::value_type>(fdp.ConsumeIntegral<uint8_t>());
    }
    if (fdp.ConsumeBool() && !payload.empty()) {
        payload.resize(fdp.ConsumeIntegralInRange<size_t>(0, payload.size() - 1));
    }
    try {
        SendMessage(*g_peerman, peer, msg_type, std::move(payload));
    } catch (const std::ios_base::failure&) {
    }
}

void Response(FuzzedDataProvider& fdp, CNode& peer) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    dsl::CPoSeServiceResponse resp;
    resp.nEpoch = PickEpoch(fdp);
    resp.proTxHash = PickProTx(fdp);
    if (const CBLSSecretKey* key = PickKey(fdp, IndexOf(resp.proTxHash))) {
        resp.sig = dsl::SignChallengeResponse(*key, PickSignBase(fdp, resp.nEpoch),
                                              fdp.ConsumeBool() ? resp.proTxHash : PickProTx(fdp));
    }
    Deliver(fdp, peer, NetMsgType::POSERESPONSE, resp);
}

void Report(FuzzedDataProvider& fdp, CNode& peer) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    dsl::CPoSeServiceReport report;
    report.nEpoch = PickEpoch(fdp);
    const auto& pairs = g_fx->assigned[report.nEpoch == g_fx->epoch ? 0 : 1];
    size_t sentinel;
    if (fdp.ConsumeBool()) {
        // a pair the epoch assigned
        const auto& [s, t] = pairs[fdp.ConsumeIntegralInRange<size_t>(0, pairs.size() - 1)];
        sentinel = s;
        report.sentinelProTxHash = g_fx->protx[s];
        report.targetProTxHash = g_fx->protx[t];
    } else {
        report.sentinelProTxHash = PickProTx(fdp);
        report.targetProTxHash = PickProTx(fdp);
        sentinel = IndexOf(report.sentinelProTxHash);
    }
    report.status = fdp.ConsumeIntegral<uint8_t>();
    if (const CBLSSecretKey* key = PickKey(fdp, sentinel)) {
        report.Sign(*key, PickSignBase(fdp, report.nEpoch));
    }
    Deliver(fdp, peer, NetMsgType::POSEREPORT, report);
}

void Challenge(FuzzedDataProvider& fdp, CNode& peer) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    dsl::CPoSeServiceChallenge challenge;
    challenge.nEpoch = PickEpoch(fdp);
    Deliver(fdp, peer, NetMsgType::POSECHALLENGE, challenge);
}

void Commitment(FuzzedDataProvider& fdp, CNode& peer) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    CPoSeServiceCommitment c;
    c.nEpoch = PickEpoch(fdp);
    const int64_t boundary = (static_cast<int64_t>(c.nEpoch) + 1) * g_fx->interval;
    const int boundary_height = boundary > 0 && boundary < std::numeric_limits<int>::max() ? static_cast<int>(boundary) : 0;
    c.nVersion = fdp.ConsumeBool() ? RequiredServiceCommitmentVersion(consensus, boundary_height) : fdp.ConsumeIntegral<uint16_t>();
    const CBlockIndex* base = BaseOf(c.nEpoch);
    c.epochBlockHash = fdp.ConsumeBool() && base != nullptr ? base->GetBlockHash() : PickSignBase(fdp, c.nEpoch);
    c.llmqType = fdp.ConsumeBool() ? llmq::GetChainLocksLLMQType(consensus, boundary_height)
                                   : static_cast<Consensus::LLMQType>(fdp.ConsumeIntegral<uint8_t>());
    c.quorumHash = ConsumeUInt256(fdp);
    const size_t bits = fdp.ConsumeBool() ? MN_COUNT : fdp.ConsumeIntegralInRange<size_t>(0, 2 * MN_COUNT);
    for (size_t i = 0; i < bits; ++i) {
        const bool missed = fdp.ConsumeBool();
        c.missed.push_back(missed);
        if (c.nVersion >= CPoSeServiceCommitment::OBSERVED_VERSION) c.observed.push_back(missed || fdp.ConsumeBool());
    }
    if (fdp.ConsumeBool()) {
        // a signature that decodes: some operator key's over the message hash (a quorum's is out of reach here)
        c.quorumSig = g_fx->keys[PickIndex(fdp)].Sign(dsl::MakeServiceCommitmentTx(c).msgHash, /*specificLegacyScheme=*/false);
    }
    Deliver(fdp, peer, NetMsgType::POSECOMMIT, c);
}

using Sender = void (*)(FuzzedDataProvider&, CNode&);

void Run(FuzzBufferType buffer, Sender first)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    // Every input starts from a fresh service manager, in the current epoch or still in the previous one (its tick
    // for the current base not yet run), a fresh PeerManager and the same peer.
    g_fx->dslman = std::make_unique<dsl::CPoSeServiceManager>();
    if (fdp.ConsumeBool()) {
        g_fx->dslman->BeginEpoch(g_fx->epoch, g_fx->base->GetBlockHash());
    } else {
        g_fx->dslman->BeginEpoch(g_fx->epoch - 1, g_fx->prev_base->GetBlockHash());
    }
    const auto& node = g_fx->setup->m_node;
    const auto peerman = PeerManager::make(Params(), *node.connman, *node.addrman, node.banman.get(), *node.chainman,
                                           *node.mempool, *node.mn_metaman, *node.mn_sync, *node.govman,
                                           *node.sporkman, g_fx->activeman.get(), g_fx->dmnman, node.cj_ctx,
                                           node.llmq_ctx, g_fx->dslman, /*ignore_incoming_txs=*/false);
    g_peerman = peerman.get();
    auto peer = MakeTestPeer(/*id=*/0);
    LOCK(NetEventsInterface::g_msgproc_mutex);
    peerman->InitializeNode(*peer, NODE_NETWORK);
    // The named message first, then any of the four: a response before a report, a duplicate after its original.
    first(fdp, *peer);
    for (int sent = 1; sent < MAX_MESSAGES && fdp.ConsumeBool(); ++sent) {
        switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
        case 0: Response(fdp, *peer); break;
        case 1: Report(fdp, *peer); break;
        case 2: Challenge(fdp, *peer); break;
        default: Commitment(fdp, *peer); break;
        }
    }
    peerman->FinalizeNode(*peer);
    g_peerman = nullptr;
}

} // namespace

FUZZ_TARGET_INIT(pose_service_deep_response, initialize_pose_service_deep) { Run(buffer, Response); }
FUZZ_TARGET_INIT(pose_service_deep_report, initialize_pose_service_deep) { Run(buffer, Report); }
FUZZ_TARGET_INIT(pose_service_deep_challenge, initialize_pose_service_deep) { Run(buffer, Challenge); }
FUZZ_TARGET_INIT(pose_service_deep_commitment, initialize_pose_service_deep) { Run(buffer, Commitment); }
