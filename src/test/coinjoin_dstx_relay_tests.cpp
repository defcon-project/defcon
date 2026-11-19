// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// CJ4-A: how a v23.1 node relays a mixing transaction (DSTX) that carries a
// DFCN-scale denomination (100.001, 1000.01 or 10 000.1 DFCN), through the real
// PeerManager. A v23.0.0 peer (protocol 70242) gives +10 for such an envelope,
// so it is never offered one: not in the trickle, not in a BIP35 mempool reply,
// not on GETDATA as MSG_DSTX. A 70243 peer gets it like any DSTX. Nothing else
// changes for the old peer: the transaction's InstantSend lock, the plain
// transaction once it may be served unconditionally, what its Bloom filter
// learns, ordinary payments, and old-amount DSTXes.

#include <arith_uint256.h>
#include <bls/bls.h>
#include <chainparams.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <coinjoin/context.h>
#include <common/bloom.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/pose_service_manager.h>
#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <llmq/context.h>
#include <llmq/instantsend.h>
#include <masternode/meta.h>
#include <net.h>
#include <net_processing.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <spork.h>
#include <streams.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <timedata.h>
#include <txmempool.h>
#include <util/check.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

namespace llmq {
// Defined in instantsend_queue_tests.cpp, which holds the manager's test seam: stores a lock as a verified one.
void StoreInstantSendLockForTest(CInstantSendManager& manager, const CInstantSendLock& islock);
} // namespace llmq

namespace {
const CAmount NEW_AMOUNT{CoinJoin::DenominationToAmount(1 << 5)}; // 100.001 DFCN
const CAmount OLD_AMOUNT{CoinJoin::DenominationToAmount(1 << 0)}; // 10.0001 DFCN
constexpr int OLD_VERSION{COINJOIN_DFCN_DENOMS_PROTO_VERSION - 1}; // v23.0.0
// net_processing.cpp's UNCONDITIONAL_RELAY_DELAY: after it a transaction is served on any GETDATA.
constexpr int64_t UNCONDITIONAL_RELAY_DELAY_SECONDS{2 * 60};
// Comfortably past an inbound peer's trickle timer (an exponential draw around five seconds), and still
// inside UNCONDITIONAL_RELAY_DELAY_SECONDS.
constexpr int64_t TRICKLE_STEP_SECONDS{100};
const std::atomic<bool> g_interrupt{false};

struct Funded {
    COutPoint outpoint;
    CAmount value;
};

struct DstxRelaySetup : RegTestingSetup {
    std::unique_ptr<CDeterministicMNManager> dmnman;
    std::unique_ptr<dsl::CPoSeServiceManager> dslman{std::make_unique<dsl::CPoSeServiceManager>()};
    std::unique_ptr<PeerManager> peerman;
    // Declared after the peer manager and what it reads, so it is destroyed before them: when the constructor fails
    // after a peer was added (and this fixture's destructor does not run), the connman's own teardown finalizes that
    // peer through a peer manager that still exists.
    ConnmanTestMsg connman{0x1337, 0x1337, *m_node.addrman, *m_node.netgroupman};
    CBLSSecretKey operator_key;
    const uint256 protx{ArithToUint256(arith_uint256{1})};
    CKey coin_key;
    FillableSigningProvider keystore;
    std::map<COutPoint, Coin> coins; // what the test's own transactions spend
    std::vector<COutPoint> coinbases;
    size_t next_coinbase{0};
    NodeId next_id{0};
    CNode* sender{nullptr};   // 70243: hands the node every transaction
    CNode* peer_new{nullptr}; // 70243
    CNode* peer_old{nullptr}; // 70242

    DstxRelaySetup() : RegTestingSetup{{"-dip3params=2:2"}}
    {
        coin_key.MakeNewKey(true);
        keystore.AddKey(coin_key);
        operator_key.MakeNewKey();
        const CScript coinbase_script{GetScriptForDestination(PKHash(coin_key.GetPubKey()))};
        for (int i = 0; i < COINBASE_MATURITY + 15; ++i) {
            coinbases.push_back(MineBlock(m_node, coinbase_script).prevout);
        }
        SyncWithValidationInterfaceQueue();
        static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).JumpOutOfIbd();
        BOOST_REQUIRE(m_node.mn_metaman->LoadCache(false));

        // A deterministic-MN reader of its own (as in dsl_report_wire_tests): the chainstate keeps the live one,
        // and the synthetic list is handed to this one through evodb once the test has mined its last block.
        dmnman = std::make_unique<CDeterministicMNManager>(m_node.chainman->ActiveChainstate(), *m_node.evodb);
        peerman = PeerManager::make(Params(), connman, *m_node.addrman, m_node.banman.get(), *m_node.chainman,
                                    *m_node.mempool, *m_node.mn_metaman, *m_node.mn_sync, *m_node.govman,
                                    *m_node.sporkman, nullptr, dmnman, m_node.cj_ctx, m_node.llmq_ctx, dslman, false);
        CConnman::Options options;
        options.m_msgproc = peerman.get();
        // The default of Options is no send buffer at all, which would pause every peer after one message.
        options.nSendBufferMaxSize = 1000 * DEFAULT_MAXSENDBUFFER;
        connman.Init(options);
        sender = Connect(PROTOCOL_VERSION);
        peer_new = Connect(PROTOCOL_VERSION);
        peer_old = Connect(OLD_VERSION);
    }

    ~DstxRelaySetup()
    {
        SyncWithValidationInterfaceQueue();
        for (CNode* node : connman.TestNodes()) peerman->FinalizeNode(*node);
        connman.ClearTestNodes();
    }

    // An inbound peer at `version` that relays transactions and may send MEMPOOL and FILTERLOAD.
    CNode* Connect(int version)
    {
        const NodeId id{next_id++};
        in_addr addr{};
        addr.s_addr = htonl(0x0a000001 + static_cast<uint32_t>(id));
        auto* node = new CNode{id,
                               /*sock=*/nullptr,
                               CAddress{CService{addr, 9999}, NODE_NETWORK},
                               /*nKeyedNetGroupIn=*/0,
                               /*nLocalHostNonceIn=*/0,
                               CAddress{},
                               /*addrNameIn=*/"",
                               ConnectionType::INBOUND,
                               /*inbound_onion=*/false};
        connman.AddTestNode(*node);
        LOCK(NetEventsInterface::g_msgproc_mutex);
        connman.Handshake(*node, /*successfully_connected=*/true, ServiceFlags(NODE_NETWORK),
                          ServiceFlags(NODE_NETWORK | NODE_BLOOM), version, /*relay_txs=*/true);
        BOOST_REQUIRE(!node->fDisconnect);
        return node;
    }

    // One confirmed output per amount, all to coin_key, from mature coinbases. The last block a test mines.
    std::vector<Funded> Fund(const std::vector<CAmount>& amounts)
    {
        const CScript script{GetScriptForDestination(PKHash(coin_key.GetPubKey()))};
        const CAmount fee{COIN / 100};
        const CAmount needed{std::accumulate(amounts.begin(), amounts.end(), CAmount{0}) + fee};
        CMutableTransaction mtx;
        CAmount in{0};
        while (in < needed) {
            BOOST_REQUIRE_LT(next_coinbase, coinbases.size() - COINBASE_MATURITY);
            const COutPoint& outpoint{coinbases[next_coinbase++]};
            Coin coin;
            BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().CoinsTip().GetCoin(outpoint, coin)));
            BOOST_REQUIRE(coin.out.scriptPubKey == script);
            coins.emplace(outpoint, coin);
            mtx.vin.emplace_back(outpoint);
            in += coin.out.nValue;
        }
        for (const CAmount amount : amounts) mtx.vout.emplace_back(amount, script);
        if (in > needed) mtx.vout.emplace_back(in - needed, script);
        Sign(mtx, SIGHASH_ALL);
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ProcessTransaction(tx).m_result_type) ==
                      MempoolAcceptResult::ResultType::VALID);
        MineBlock(m_node, script);
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE(!m_node.mempool->exists(tx->GetHash()));
        std::vector<Funded> funded;
        for (size_t i = 0; i < amounts.size(); ++i) {
            const COutPoint outpoint{tx->GetHash(), static_cast<uint32_t>(i)};
            coins.emplace(outpoint, Coin{tx->vout[i], 1, /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/false});
            funded.push_back({outpoint, amounts[i]});
        }
        return funded;
    }

    void Sign(CMutableTransaction& mtx, int sighash)
    {
        std::map<int, bilingual_str> errors;
        BOOST_REQUIRE(SignTransaction(mtx, &keystore, coins, sighash, errors));
    }

    // The masternode that signs the test's DSTXes, in the list at the tip, with the operator key of the test.
    // Called after the test's last block, so the live manager never sees this list.
    void InstallMasternode()
    {
        const CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
        CDeterministicMNList list{tip->GetBlockHash(), tip->nHeight, 1};
        auto mn = std::make_shared<CDeterministicMN>(0);
        mn->proTxHash = protx;
        mn->collateralOutpoint = COutPoint{protx, 0};
        auto state = std::make_shared<CDeterministicMNState>();
        state->nRegisteredHeight = 1;
        CKey owner_key;
        owner_key.MakeNewKey(true);
        state->keyIDOwner = owner_key.GetPubKey().GetID();
        state->pubKeyOperator.Set(operator_key.GetPublicKey(), /*specificLegacyScheme=*/false);
        mn->pdmnState = state;
        list.AddMN(mn);
        m_node.evodb->Write(std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), tip->GetBlockHash()), list);
        dmnman->UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman->GetListForBlock(tip).GetMN(protx));
    }

    // A mixing transaction of the funded coins, each paid on to a fresh key at its own amount (in the funded
    // order), signed as the client signs, in an envelope signed by the test's masternode.
    CCoinJoinBroadcastTx MakeDstx(const std::vector<Funded>& inputs, std::vector<CKey>* keys_out = nullptr)
    {
        CMutableTransaction mtx;
        for (const auto& input : inputs) {
            CKey key;
            key.MakeNewKey(true);
            keystore.AddKey(key);
            if (keys_out) keys_out->push_back(key);
            mtx.vin.emplace_back(input.outpoint);
            mtx.vout.emplace_back(input.value, GetScriptForDestination(PKHash(key.GetPubKey())));
        }
        Sign(mtx, SIGHASH_ALL | SIGHASH_ANYONECANPAY);
        CCoinJoinBroadcastTx dstx{MakeTransactionRef(mtx), COutPoint{}, protx, GetAdjustedTime()};
        dstx.vchSig = operator_key.Sign(dstx.GetSignatureHash(), /*specificLegacyScheme=*/false).ToByteVector(false);
        BOOST_REQUIRE(dstx.IsValidStructure());
        BOOST_REQUIRE(dstx.CheckSignature(operator_key.GetPublicKey()));
        for (size_t i = 0; i < mtx.vout.size(); ++i) {
            coins.emplace(COutPoint{mtx.GetHash(), static_cast<uint32_t>(i)},
                          Coin(mtx.vout[i], MEMPOOL_HEIGHT, /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/false));
        }
        return dstx;
    }

    // An ordinary payment with a fee: `amount` to a fresh key, the rest back to coin_key.
    CTransactionRef MakePayment(const Funded& input, CAmount amount)
    {
        CKey key;
        key.MakeNewKey(true);
        CMutableTransaction mtx;
        mtx.vin.emplace_back(input.outpoint);
        mtx.vout.emplace_back(amount, GetScriptForDestination(PKHash(key.GetPubKey())));
        const CAmount change{input.value - amount - COIN / 1000};
        if (change > 0) mtx.vout.emplace_back(change, GetScriptForDestination(PKHash(coin_key.GetPubKey())));
        Sign(mtx, SIGHASH_ALL);
        return MakeTransactionRef(mtx);
    }

    void Deliver(CNode& from, const std::string& type, CDataStream& stream)
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->ProcessMessage(from, type, stream, GetTime<std::chrono::microseconds>(), g_interrupt);
    }

    // The sender hands the node a DSTX, which is accepted, prioritised and kept.
    void Submit(const CCoinJoinBroadcastTx& dstx)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << dstx;
        Deliver(*sender, NetMsgType::DSTX, stream);
        const uint256 hash{dstx.tx->GetHash()};
        BOOST_REQUIRE(m_node.mempool->exists(hash));
        BOOST_REQUIRE(static_cast<bool>(m_node.cj_ctx->dstxman->GetDSTX(hash)));
        CAmount delta{0};
        WITH_LOCK(m_node.mempool->cs, m_node.mempool->ApplyDelta(hash, delta));
        BOOST_REQUIRE_EQUAL(delta, COIN / 10);
        BOOST_REQUIRE_EQUAL(MisbehaviorScore(*peerman, *sender), 0);
    }

    void Submit(const CTransactionRef& tx)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << tx;
        Deliver(*sender, NetMsgType::TX, stream);
        BOOST_REQUIRE(m_node.mempool->exists(tx->GetHash()));
    }

    void Elapse(int64_t seconds) { SetMockTime(GetTime() + seconds); }

    void Trickle(CNode& node)
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        peerman->SendMessages(&node);
    }

    void AskMempool(CNode& node)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        Deliver(node, NetMsgType::MEMPOOL, stream);
        BOOST_REQUIRE(!node.fDisconnect);
    }

    void GetData(CNode& node, int type, const uint256& hash)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << std::vector<CInv>{CInv(type, hash)};
        node.fPauseSend = false; // nothing drains the send buffer here; GETDATA stops at a paused peer
        Deliver(node, NetMsgType::GETDATA, stream);
    }

    void Clear(CNode& node) const { connman.FlushSendBuffer(node); }

    // The inventory items of every message of `type` (INV or NOTFOUND) queued for `node`.
    std::vector<CInv> Items(CNode& node, const std::string& type) const
    {
        std::vector<CInv> items;
        LOCK(node.cs_vSend);
        for (const auto& msg : node.vSendMsg) {
            if (msg.m_type != type) continue;
            CDataStream stream{MakeUCharSpan(msg.data), SER_NETWORK, node.GetCommonVersion()};
            std::vector<CInv> part;
            stream >> part;
            items.insert(items.end(), part.begin(), part.end());
        }
        return items;
    }
    bool Offered(CNode& node, int type, const uint256& hash) const
    {
        const auto items{Items(node, NetMsgType::INV)};
        return std::any_of(items.begin(), items.end(), [&](const CInv& inv) {
            return inv.type == static_cast<uint32_t>(type) && inv.hash == hash;
        });
    }
    bool OfferedAny(CNode& node, const uint256& hash) const
    {
        const auto items{Items(node, NetMsgType::INV)};
        return std::any_of(items.begin(), items.end(), [&](const CInv& inv) { return inv.hash == hash; });
    }
    // A NOTFOUND item of the requested type and hash: the requester matches its pending request by the whole CInv.
    bool NotFound(CNode& node, int type, const uint256& hash) const
    {
        const auto items{Items(node, NetMsgType::NOTFOUND)};
        return std::any_of(items.begin(), items.end(), [&](const CInv& inv) {
            return inv.type == static_cast<uint32_t>(type) && inv.hash == hash;
        });
    }

    // The hashes of the transactions sent to `node` as TX messages.
    std::vector<uint256> SentTxs(CNode& node) const
    {
        std::vector<uint256> hashes;
        LOCK(node.cs_vSend);
        for (const auto& msg : node.vSendMsg) {
            if (msg.m_type != NetMsgType::TX) continue;
            CDataStream stream{MakeUCharSpan(msg.data), SER_NETWORK, node.GetCommonVersion()};
            CMutableTransaction mtx;
            stream >> mtx;
            hashes.push_back(mtx.GetHash());
        }
        return hashes;
    }
    bool SentTx(CNode& node, const uint256& hash) const
    {
        const auto hashes{SentTxs(node)};
        return std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
    }

    // The DSTX messages sent to `node`.
    std::vector<CCoinJoinBroadcastTx> SentDstxs(CNode& node) const
    {
        std::vector<CCoinJoinBroadcastTx> dstxs;
        LOCK(node.cs_vSend);
        for (const auto& msg : node.vSendMsg) {
            if (msg.m_type != NetMsgType::DSTX) continue;
            CDataStream stream{MakeUCharSpan(msg.data), SER_NETWORK, node.GetCommonVersion()};
            CCoinJoinBroadcastTx dstx;
            stream >> dstx;
            dstxs.push_back(dstx);
        }
        return dstxs;
    }
    // The node sent `node` this DSTX: the same transaction, from the test's masternode, with a valid signature.
    bool SentDstx(CNode& node, const uint256& hash) const
    {
        const auto dstxs{SentDstxs(node)};
        return std::any_of(dstxs.begin(), dstxs.end(), [&](const CCoinJoinBroadcastTx& dstx) {
            return dstx.tx && dstx.tx->GetHash() == hash && dstx.m_protxHash == protx &&
                   dstx.CheckSignature(operator_key.GetPublicKey());
        });
    }
    // Nothing that carries this transaction was sent to `node`: neither a DSTX nor a TX message.
    bool SentNothingOf(CNode& node, const uint256& hash) const
    {
        const auto dstxs{SentDstxs(node)};
        return !SentTx(node, hash) && std::none_of(dstxs.begin(), dstxs.end(), [&](const CCoinJoinBroadcastTx& dstx) {
            return dstx.tx && dstx.tx->GetHash() == hash;
        });
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(coinjoin_dstx_relay_tests, DstxRelaySetup)

// Note 5 (a), (b), (d), (d2) and r7/R17: the trickle and GETDATA, for a DSTX of a new amount only and for one with
// a new amount after an old one.
BOOST_AUTO_TEST_CASE(new_amount_dstx_reaches_only_70243_peers)
{
    const auto funded{Fund({NEW_AMOUNT, NEW_AMOUNT, NEW_AMOUNT, OLD_AMOUNT, NEW_AMOUNT, NEW_AMOUNT, 3 * NEW_AMOUNT})};
    InstallMasternode();
    const auto only_new{MakeDstx({funded[0], funded[1], funded[2]})};
    const auto mixed{MakeDstx({funded[3], funded[4], funded[5]})};
    // The control payment spends the change of an unconfirmed ordinary parent: two ancestors in the mempool against
    // the DSTXes' one. The trickle orders by ancestor count first (and only then by base fee, where the DSTXes'
    // priority does not count), so it takes the payment after both DSTXes.
    const auto parent{MakePayment(funded[6], COIN)};
    BOOST_REQUIRE_EQUAL(parent->vout.size(), 2U);
    coins.emplace(COutPoint{parent->GetHash(), 1}, Coin{parent->vout[1], MEMPOOL_HEIGHT, /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/false});
    const auto payment{MakePayment({COutPoint{parent->GetHash(), 1}, parent->vout[1].nValue}, NEW_AMOUNT)};
    BOOST_REQUIRE(only_new.HasDfcnScaleDenomination());
    BOOST_REQUIRE(mixed.HasDfcnScaleDenomination());
    BOOST_REQUIRE_EQUAL(mixed.tx->vout[0].nValue, OLD_AMOUNT);

    // (a) Accepted from a masternode the node knows, prioritised and kept, at any height (H is unset here).
    Submit(only_new);
    Submit(mixed);
    Submit(parent);
    Submit(payment);
    const uint256 only_new_hash{only_new.tx->GetHash()};
    const uint256 mixed_hash{mixed.tx->GetHash()};
    const uint256 payment_hash{payment->GetHash()};
    // So on the 70242 connection the payment comes after both skipped items: skipping one must not end or delay
    // that peer's trickle.
    BOOST_REQUIRE(m_node.mempool->CompareDepthAndScore(only_new_hash, payment_hash));
    BOOST_REQUIRE(m_node.mempool->CompareDepthAndScore(mixed_hash, payment_hash));

    // (b) The trickle: the 70243 peer is offered both, the 70242 peer neither, and in the same trickle the payment.
    Clear(*peer_new);
    Clear(*peer_old);
    Elapse(TRICKLE_STEP_SECONDS);
    Trickle(*peer_new);
    Trickle(*peer_old);
    BOOST_CHECK(Offered(*peer_new, MSG_DSTX, only_new_hash));
    BOOST_CHECK(Offered(*peer_new, MSG_DSTX, mixed_hash));
    BOOST_CHECK(!OfferedAny(*peer_old, only_new_hash));
    BOOST_CHECK(!OfferedAny(*peer_old, mixed_hash));
    BOOST_CHECK_MESSAGE(Offered(*peer_old, MSG_TX, payment_hash),
                        "the payment after the skipped DSTXes reaches the 70242 peer in the same trickle");

    // r7/R20: the payment has no DSTX, so the 70242 peer asking for it as MSG_DSTX gets the plain transaction.
    Clear(*peer_old);
    GetData(*peer_old, MSG_DSTX, payment_hash);
    BOOST_CHECK(SentTx(*peer_old, payment_hash));
    BOOST_CHECK(SentDstxs(*peer_old).empty());

    // Right after the trickle and before the unconditional relay delay, the skipped announcement left nothing the
    // old peer could fetch: NOTFOUND as MSG_TX and as MSG_DSTX.
    for (const uint256& hash : {only_new_hash, mixed_hash}) {
        for (const int type : {MSG_TX, MSG_DSTX}) {
            Clear(*peer_old);
            GetData(*peer_old, type, hash);
            BOOST_CHECK(NotFound(*peer_old, type, hash));
            BOOST_CHECK(SentNothingOf(*peer_old, hash));
        }
        // r7/R17: the announced 70243 peer fetches the DSTX at once, from the masternode, correctly signed.
        Clear(*peer_new);
        GetData(*peer_new, MSG_DSTX, hash);
        BOOST_CHECK(SentDstx(*peer_new, hash));
    }

    // (d) Once the transaction may be served unconditionally: the DSTX to the 70243 peer; to the 70242 peer
    // NOTFOUND as MSG_DSTX and the plain transaction as MSG_TX.
    Elapse(UNCONDITIONAL_RELAY_DELAY_SECONDS);
    for (const uint256& hash : {only_new_hash, mixed_hash}) {
        Clear(*peer_new);
        GetData(*peer_new, MSG_DSTX, hash);
        BOOST_CHECK(SentDstx(*peer_new, hash));
        Clear(*peer_old);
        GetData(*peer_old, MSG_DSTX, hash);
        BOOST_CHECK_MESSAGE(NotFound(*peer_old, MSG_DSTX, hash),
                            "a held-back DSTX asked for as MSG_DSTX after the delay is NOTFOUND for the 70242 peer");
        BOOST_CHECK(SentNothingOf(*peer_old, hash));
        Clear(*peer_old);
        GetData(*peer_old, MSG_TX, hash);
        BOOST_CHECK(SentTx(*peer_old, hash));
        BOOST_CHECK(SentDstxs(*peer_old).empty());
    }
    BOOST_CHECK_EQUAL(MisbehaviorScore(*peerman, *sender), 0);
}

// Note 5 (c), (d2), (d4), (e): the BIP35 reply, observed on its own by peers that connect after the transactions
// were relayed (so nothing reaches them through the trickle).
BOOST_AUTO_TEST_CASE(bip35_reply_holds_back_only_new_amount_dstxes)
{
    const auto funded{Fund({NEW_AMOUNT, NEW_AMOUNT, NEW_AMOUNT, OLD_AMOUNT, OLD_AMOUNT, OLD_AMOUNT,
                            OLD_AMOUNT, NEW_AMOUNT, NEW_AMOUNT, 3 * NEW_AMOUNT})};
    InstallMasternode();
    const auto only_new{MakeDstx({funded[0], funded[1], funded[2]})};
    const auto only_old{MakeDstx({funded[3], funded[4], funded[5]})};
    const auto mixed{MakeDstx({funded[6], funded[7], funded[8]})};
    const auto payment{MakePayment(funded[9], NEW_AMOUNT)};
    BOOST_REQUIRE(!only_old.HasDfcnScaleDenomination());
    Submit(only_new);
    Submit(only_old);
    Submit(mixed);
    Submit(payment);

    CNode& late_new{*Connect(PROTOCOL_VERSION)};
    CNode& late_old{*Connect(OLD_VERSION)};
    AskMempool(late_new);
    AskMempool(late_old);
    Clear(late_new);
    Clear(late_old);
    Elapse(TRICKLE_STEP_SECONDS);
    Trickle(late_new);
    Trickle(late_old);

    BOOST_CHECK(Offered(late_new, MSG_DSTX, only_new.tx->GetHash()));
    BOOST_CHECK(Offered(late_new, MSG_DSTX, only_old.tx->GetHash()));
    BOOST_CHECK(Offered(late_new, MSG_DSTX, mixed.tx->GetHash()));
    BOOST_CHECK(Offered(late_new, MSG_TX, payment->GetHash()));

    BOOST_CHECK_MESSAGE(!OfferedAny(late_old, only_new.tx->GetHash()),
                        "the BIP35 reply holds a new-amount DSTX back from the 70242 peer");
    BOOST_CHECK(!OfferedAny(late_old, mixed.tx->GetHash()));
    // (e) an old-amount DSTX and (d4) an ordinary payment with a 100.001 DFCN output: as before.
    BOOST_CHECK(Offered(late_old, MSG_DSTX, only_old.tx->GetHash()));
    BOOST_CHECK(Offered(late_old, MSG_TX, payment->GetHash()));
}

// Note 5 (c), r5/R4, r6/R14: the BIP35 reply skips only the held-back DSTX's own item; its InstantSend lock is still
// offered. The observers connect after the lock was stored, so the lock can only reach them through the reply.
BOOST_AUTO_TEST_CASE(bip35_reply_still_offers_the_lock_of_a_held_back_dstx)
{
    const auto funded{Fund({NEW_AMOUNT, NEW_AMOUNT, NEW_AMOUNT})};
    InstallMasternode();
    const auto only_new{MakeDstx({funded[0], funded[1], funded[2]})};
    Submit(only_new);

    CKey spork_key;
    spork_key.MakeNewKey(false);
    auto& sporkman{*m_node.sporkman};
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash(spork_key.GetPubKey()))));
    BOOST_REQUIRE(sporkman.SetMinSporkKeys(1));
    BOOST_REQUIRE(sporkman.SetPrivKey(EncodeSecret(spork_key)));
    BOOST_REQUIRE(sporkman.UpdateSpork(*Assert(m_node.peerman.get()), SPORK_2_INSTANTSEND_ENABLED, 0));

    llmq::CInstantSendLock islock;
    islock.txid = only_new.tx->GetHash();
    for (const auto& in : only_new.tx->vin) islock.inputs.push_back(in.prevout);
    islock.cycleHash = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    llmq::StoreInstantSendLockForTest(*m_node.llmq_ctx->isman, islock);
    BOOST_REQUIRE(m_node.llmq_ctx->isman->GetInstantSendLockByTxid(islock.txid));
    const uint256 lock_hash{::SerializeHash(islock)};

    CNode& late_new{*Connect(PROTOCOL_VERSION)};
    CNode& late_old{*Connect(OLD_VERSION)};
    AskMempool(late_new);
    AskMempool(late_old);
    Clear(late_new);
    Clear(late_old);
    Elapse(TRICKLE_STEP_SECONDS);
    Trickle(late_new);
    Trickle(late_old);

    BOOST_CHECK(Offered(late_new, MSG_DSTX, islock.txid));
    BOOST_CHECK(Offered(late_new, MSG_ISDLOCK, lock_hash));
    BOOST_CHECK(!OfferedAny(late_old, islock.txid));
    BOOST_CHECK(Offered(late_old, MSG_ISDLOCK, lock_hash));
}

// Note 5 (d3), r6/R9: the old peer's Bloom filter sees a held-back DSTX before it is skipped, so a later ordinary
// transaction that matches only by spending one of its outputs still reaches that peer. A second 70242 peer with the
// same filter that does not update (BLOOM_UPDATE_NONE) is not offered that spend, so the first peer's match comes from
// the outpoint its filter learned, not from a trickle that skipped the filter.
BOOST_AUTO_TEST_CASE(bloom_filter_still_learns_from_a_held_back_dstx)
{
    const auto funded{Fund({NEW_AMOUNT, NEW_AMOUNT, NEW_AMOUNT})};
    InstallMasternode();
    std::vector<CKey> keys;
    const auto only_new{MakeDstx({funded[0], funded[1], funded[2]}, &keys)};
    CNode& peer_fixed{*Connect(OLD_VERSION)};

    for (const auto& [node, flags] : {std::pair<CNode*, unsigned char>{peer_old, BLOOM_UPDATE_ALL},
                                      std::pair<CNode*, unsigned char>{&peer_fixed, BLOOM_UPDATE_NONE}}) {
        CBloomFilter filter{10, 0.000001, 0, flags};
        filter.insert(ToByteVector(PKHash(keys[0].GetPubKey())));
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << filter;
        Deliver(*node, NetMsgType::FILTERLOAD, stream);
        BOOST_REQUIRE(!node->fDisconnect);
    }

    Submit(only_new);
    Clear(*peer_old);
    Clear(peer_fixed);
    Elapse(TRICKLE_STEP_SECONDS);
    Trickle(*peer_old);
    Trickle(peer_fixed);
    BOOST_CHECK(!OfferedAny(*peer_old, only_new.tx->GetHash()));
    BOOST_CHECK(!OfferedAny(peer_fixed, only_new.tx->GetHash()));

    // A payment from the DSTX's first output to a key the filter does not hold: it matches only through the
    // outpoint the filter recorded when it saw the DSTX.
    const auto spend{MakePayment({COutPoint{only_new.tx->GetHash(), 0}, NEW_AMOUNT}, NEW_AMOUNT - COIN / 1000)};
    BOOST_REQUIRE_EQUAL(spend->vout.size(), 1U);
    Submit(spend);
    Clear(*peer_old);
    Clear(peer_fixed);
    Elapse(TRICKLE_STEP_SECONDS);
    Trickle(*peer_old);
    Trickle(peer_fixed);
    BOOST_CHECK(Offered(*peer_old, MSG_TX, spend->GetHash()));
    BOOST_CHECK_MESSAGE(!OfferedAny(peer_fixed, spend->GetHash()),
                        "a 70242 peer whose filter does not update is not offered the spend");
}

// Note 5 (d4), (e): through the trickle, an old-amount DSTX and an ordinary payment with a 100.001 DFCN output reach
// both peers as before, and the old-amount DSTX is fetched as a DSTX by both.
BOOST_AUTO_TEST_CASE(old_amount_dstx_and_ordinary_payment_reach_both_peers)
{
    const auto funded{Fund({OLD_AMOUNT, OLD_AMOUNT, OLD_AMOUNT, 3 * NEW_AMOUNT})};
    InstallMasternode();
    const auto only_old{MakeDstx({funded[0], funded[1], funded[2]})};
    const auto payment{MakePayment(funded[3], NEW_AMOUNT)};
    Submit(only_old);
    Submit(payment);

    Clear(*peer_new);
    Clear(*peer_old);
    Elapse(TRICKLE_STEP_SECONDS);
    Trickle(*peer_new);
    Trickle(*peer_old);
    for (CNode* peer : {peer_new, peer_old}) {
        BOOST_CHECK(Offered(*peer, MSG_DSTX, only_old.tx->GetHash()));
        BOOST_CHECK(Offered(*peer, MSG_TX, payment->GetHash()));
        Clear(*peer);
        GetData(*peer, MSG_DSTX, only_old.tx->GetHash());
        BOOST_CHECK(SentDstx(*peer, only_old.tx->GetHash()));
    }
}

BOOST_AUTO_TEST_SUITE_END()
