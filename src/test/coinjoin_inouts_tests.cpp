// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/node.h>
#include <coinjoin/context.h>
#include <bls/bls.h>
#include <chain.h>
#include <coins.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <coinjoin/server.h>
#include <evo/chainhelper.h>
#include <llmq/context.h>
#include <llmq/instantsend.h>
#include <masternode/sync.h>
#include <net.h>
#include <netfulfilledman.h>
#include <node/connection_types.h>
#include <protocol.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(coinjoin_inouts_tests, TestingSetup)

static CBLSSecretKey MakeSecretKey()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk;
}

static CScript P2PKHScript(uint8_t tag = 0x01)
{
    // OP_DUP OP_HASH160 <20-byte-tag> OP_EQUALVERIFY OP_CHECKSIG
    std::vector<unsigned char> hash(20, tag);
    return CScript{} << OP_DUP << OP_HASH160 << hash << OP_EQUALVERIFY << OP_CHECKSIG;
}

BOOST_AUTO_TEST_CASE(broadcasttx_isvalidstructure_good_and_bad)
{
    // Good: equal vin/vout sizes, vin count >= min participants, <= max*entry_size, P2PKH outputs with standard denominations
    CCoinJoinBroadcastTx good;
    {
        CMutableTransaction mtx;
        // Use min pool participants (e.g. 3). Build 3 inputs and 3 denominated outputs
        const int participants = std::max(3, CoinJoin::GetMinPoolParticipants());
        for (int i = 0; i < participants; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            // Pick the smallest denomination
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i))};
            mtx.vout.push_back(out);
        }
        good.tx = MakeTransactionRef(mtx);
        good.m_protxHash = uint256::ONE; // at least one of (outpoint, protxhash) must be set
    }
    BOOST_CHECK(good.IsValidStructure());

    // Bad: both identifiers null
    CCoinJoinBroadcastTx bad_ids = good;
    bad_ids.m_protxHash = uint256{};
    bad_ids.masternodeOutpoint.SetNull();
    BOOST_CHECK(!bad_ids.IsValidStructure());

    // Bad: vin/vout size mismatch
    CCoinJoinBroadcastTx bad_sizes = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout.pop_back();
        bad_sizes.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_sizes.IsValidStructure());

    // Bad: non-P2PKH output
    CCoinJoinBroadcastTx bad_script = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'x'};
        bad_script.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_script.IsValidStructure());

    // Bad: non-denominated amount
    CCoinJoinBroadcastTx bad_amount = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].nValue = 42; // not a valid denom
        bad_amount.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_amount.IsValidStructure());
}

BOOST_AUTO_TEST_CASE(entry_addscriptsig_matches_and_rejects)
{
    // Build an entry with two distinct inputs so we can check both the match
    // and the isolation (only the matching input mutates).
    const COutPoint op0(uint256::ONE, 0);
    const COutPoint op1(uint256::ONE, 1);
    const uint32_t seq0 = 0xfffffffeU;
    const uint32_t seq1 = 0xfffffffdU;

    auto make_dsin = [](const COutPoint& op, uint32_t seq) {
        CTxIn in(op);
        in.nSequence = seq;
        return CTxDSIn(in, P2PKHScript(0x10), /*nRounds=*/0);
    };

    std::vector<CTxDSIn> dsins{make_dsin(op0, seq0), make_dsin(op1, seq1)};
    CCoinJoinEntry entry(dsins, /*vecTxOut=*/{}, CTransaction{CMutableTransaction{}});

    // The scriptSig we expect to be copied across on a successful match.
    const CScript scriptSig0 = CScript() << std::vector<unsigned char>{0xde, 0xad} << std::vector<unsigned char>{0xbe, 0xef};

    // Matching prevout + matching sequence -> copies scriptSig, sets fHasSig.
    {
        CTxIn signed_in(op0, scriptSig0, seq0);
        BOOST_CHECK(entry.AddScriptSig(signed_in));
        BOOST_CHECK(entry.vecTxDSIn[0].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[0].scriptSig == scriptSig0);
        // Other input is untouched.
        BOOST_CHECK(!entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig.empty());
    }

    // Duplicate signature for the already-signed input -> rejected, no overwrite.
    {
        const CScript scriptSig_other = CScript() << std::vector<unsigned char>{0x01};
        CTxIn dup_in(op0, scriptSig_other, seq0);
        BOOST_CHECK(!entry.AddScriptSig(dup_in));
        // Still holds the original signature.
        BOOST_CHECK(entry.vecTxDSIn[0].scriptSig == scriptSig0);
        BOOST_CHECK(entry.vecTxDSIn[0].fHasSig);
    }

    // Wrong prevout (sequence matches an existing input) -> rejected.
    {
        const COutPoint op_wrong(uint256S("ff"), 9);
        CTxIn wrong_in(op_wrong, scriptSig0, seq1);
        BOOST_CHECK(!entry.AddScriptSig(wrong_in));
        BOOST_CHECK(!entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig.empty());
    }

    // Right prevout but wrong sequence -> rejected (guards against malleated nSequence).
    {
        CTxIn badseq_in(op1, scriptSig0, /*nSequence=*/seq1 ^ 0xffU);
        BOOST_CHECK(!entry.AddScriptSig(badseq_in));
        BOOST_CHECK(!entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig.empty());
    }

    // Correct prevout + correct sequence on the second input -> succeeds, doesn't disturb the first.
    {
        const CScript scriptSig1 = CScript() << std::vector<unsigned char>{0xca, 0xfe};
        CTxIn signed_in(op1, scriptSig1, seq1);
        BOOST_CHECK(entry.AddScriptSig(signed_in));
        BOOST_CHECK(entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig == scriptSig1);
        // First input unchanged.
        BOOST_CHECK(entry.vecTxDSIn[0].scriptSig == scriptSig0);
    }
}

// Test-only subclass exposing the minimal seams needed to observe how
// ProcessDSSIGNFINALTX treats messages from participants vs. non-participants
// without standing up a full DKG-backed signing session.
class TestableCoinJoinServer : public CCoinJoinServer
{
public:
    using CCoinJoinServer::CCoinJoinServer;

    void EnterSigningState() { nState = POOL_STATE_SIGNING; }

    void SeedParticipant(const CService& addr) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        CCoinJoinEntry entry;
        entry.addr = addr;
        LOCK(cs_coinjoin);
        vecEntries.push_back(std::move(entry));
    }
};

static std::unique_ptr<CNode> MakePeer(NodeId id, uint32_t ipv4)
{
    in_addr peer_in_addr{};
    peer_in_addr.s_addr = htonl(ipv4);
    auto peer = std::make_unique<CNode>(id,
                                        /*sock=*/nullptr,
                                        /*addrIn=*/CAddress{CService{peer_in_addr, 8333}, NODE_NETWORK},
                                        /*nKeyedNetGroupIn=*/0,
                                        /*nLocalHostNonceIn=*/0,
                                        /*addrBindIn=*/CAddress{},
                                        /*addrNameIn=*/std::string{},
                                        /*conn_type_in=*/ConnectionType::INBOUND,
                                        /*inbound_onion=*/false);
    peer->nVersion = PROTOCOL_VERSION;
    peer->SetCommonVersion(PROTOCOL_VERSION);
    return peer;
}

BOOST_AUTO_TEST_CASE(server_signfinaltx_nonparticipant_cannot_abort_session)
{
    BOOST_REQUIRE(m_node.mn_sync);
    BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(/*load_cache=*/false));
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(MakeSecretKey(), *Assert(m_node.connman), m_node.dmnman);
    TestableCoinJoinServer server(*Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.cj_ctx->dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), &mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman), m_node.peerman);

    // Seed an active signing session with one participant. That participant's
    // addr is deliberately not registered with connman -- a session-wide
    // RelayStatus(REJECTED) would therefore see nDisconnected == vecEntries.size()
    // and reset the pool state to POOL_STATE_IDLE via SetNull().
    auto participant = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    server.SeedParticipant(participant->addr);
    server.EnterSigningState();
    BOOST_REQUIRE_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});

    auto nonparticipant = MakePeer(/*id=*/42, /*ipv4=*/0x01020304);
    BOOST_REQUIRE(!(nonparticipant->addr == participant->addr));

    const size_t max_txins{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, max_txins + 1);

    BOOST_CHECK_NO_THROW(server.ProcessMessage(*nonparticipant, NetMsgType::DSSIGNFINALTX, stream));

    // The non-participant must be rejected without touching session-wide state:
    // the stream body must not have been read past the compact-size prefix, and
    // the pool must still be in POOL_STATE_SIGNING with its entry intact.
    BOOST_CHECK_EQUAL(stream.size(), GetSizeOfCompactSize(max_txins + 1));
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);
}

BOOST_AUTO_TEST_CASE(server_signfinaltx_participant_oversized_count_is_rejected_locally)
{
    BOOST_REQUIRE(m_node.mn_sync);
    BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(/*load_cache=*/false));
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(MakeSecretKey(), *Assert(m_node.connman), m_node.dmnman);
    TestableCoinJoinServer server(*Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.cj_ctx->dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), &mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman), m_node.peerman);

    // Same setup, but this time the oversized DSSIGNFINALTX comes from the
    // session participant itself. It must still be rejected without materializing
    // the txin vector and without collapsing the session for everyone else.
    auto participant = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    server.SeedParticipant(participant->addr);
    server.EnterSigningState();

    const size_t max_txins{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, max_txins + 1);

    BOOST_CHECK_NO_THROW(server.ProcessMessage(*participant, NetMsgType::DSSIGNFINALTX, stream));
    BOOST_CHECK_EQUAL(stream.size(), 0U);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);

    // A count beyond the generic CompactSize cap is a malformed message, not a
    // CoinJoin-level violation: it throws the standard deserialization error out
    // to net processing. No txin is materialized and the session is left intact,
    // so honest participants are unaffected.
    CDataStream huge_stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(huge_stream, uint64_t{MAX_SIZE} + 1);

    BOOST_CHECK_THROW(server.ProcessMessage(*participant, NetMsgType::DSSIGNFINALTX, huge_stream),
                      std::ios_base::failure);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);
}

BOOST_AUTO_TEST_CASE(entry_deserializes_vectors_through_wire_cap)
{
    const size_t wire_cap{CoinJoin::GetMaxPoolInputOutputCount()};
    BOOST_REQUIRE_GT(wire_cap, COINJOIN_ENTRY_MAX_SIZE);

    for (const size_t count : {size_t{0}, COINJOIN_ENTRY_MAX_SIZE, COINJOIN_ENTRY_MAX_SIZE + 1, wire_cap}) {
        BOOST_TEST_CONTEXT("count=" << count)
        {
            CCoinJoinEntry entry;
            entry.vecTxDSIn.resize(count);
            entry.vecTxOut.resize(count);

            CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
            stream << entry;

            CCoinJoinEntry roundtripped;
            BOOST_CHECK_NO_THROW(stream >> roundtripped);
            BOOST_CHECK_EQUAL(roundtripped.vecTxDSIn.size(), count);
            BOOST_CHECK_EQUAL(roundtripped.vecTxOut.size(), count);
        }
    }
}

BOOST_AUTO_TEST_CASE(entry_rejects_inputs_above_wire_cap_before_materializing)
{
    const size_t wire_cap{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, wire_cap + 1);

    CCoinJoinEntry entry;
    BOOST_CHECK_THROW(stream >> entry, std::ios_base::failure);
    BOOST_CHECK(entry.vecTxDSIn.empty());
}

BOOST_AUTO_TEST_CASE(entry_rejects_outputs_above_wire_cap_before_materializing)
{
    const size_t wire_cap{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, 0);
    stream << MakeTransactionRef(CMutableTransaction{});
    WriteCompactSize(stream, wire_cap + 1);

    CCoinJoinEntry entry;
    BOOST_CHECK_THROW(stream >> entry, std::ios_base::failure);
    BOOST_CHECK(entry.vecTxOut.empty());
}

BOOST_AUTO_TEST_CASE(queue_timeout_bounds)
{
    CCoinJoinQueue dsq;
    dsq.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    dsq.m_protxHash = uint256::ONE;
    dsq.nTime = GetAdjustedTime();
    // current time -> not out of bounds
    BOOST_CHECK(!dsq.IsTimeOutOfBounds());

    // Too old (beyond COINJOIN_QUEUE_TIMEOUT)
    SetMockTime(GetTime() + (COINJOIN_QUEUE_TIMEOUT + 1));
    BOOST_CHECK(dsq.IsTimeOutOfBounds());

    // Too far in the future
    SetMockTime(GetTime() - 2 * (COINJOIN_QUEUE_TIMEOUT + 1)); // move back to anchor baseline
    dsq.nTime = GetAdjustedTime() + (COINJOIN_QUEUE_TIMEOUT + 1);
    BOOST_CHECK(dsq.IsTimeOutOfBounds());

    // Reset mock time
    SetMockTime(0);
}

// CJ4-A: the wire table knows eight amounts, the wallet API keeps the five of v23.0.0.
BOOST_AUTO_TEST_CASE(dfcn_scale_denominations_wire_table_and_wallet_api)
{
    // Bits 0-4: the v23.0.0 values, both directions.
    const std::array<std::pair<int, CAmount>, 5> old_bits{{
        {1, 1000010000}, {2, 100001000}, {4, 10000100}, {8, 1000010}, {16, 100001}}};
    // Bits 5-7: literals independent of the product table, both directions, so a swap of two
    // table entries fails here.
    const std::array<std::pair<int, CAmount>, 3> new_bits{{
        {32, 10000100000}, {64, 100001000000}, {128, 1000010000000}}};
    for (const auto& [bit, amount] : old_bits) {
        BOOST_CHECK_EQUAL(CoinJoin::DenominationToAmount(bit), amount);
        BOOST_CHECK_EQUAL(CoinJoin::AmountToDenomination(amount), bit);
        BOOST_CHECK(CoinJoin::IsValidDenomination(bit));
        BOOST_CHECK(CoinJoin::IsKnownDenominationAmount(amount));
        BOOST_CHECK(CoinJoin::IsDenominatedAmount(amount));
        BOOST_CHECK(!CoinJoin::IsDfcnScaleDenomination(bit));
    }
    for (const auto& [bit, amount] : new_bits) {
        BOOST_CHECK_EQUAL(CoinJoin::DenominationToAmount(bit), amount);
        BOOST_CHECK_EQUAL(CoinJoin::AmountToDenomination(amount), bit);
        BOOST_CHECK(CoinJoin::IsValidDenomination(bit));
        BOOST_CHECK(CoinJoin::IsKnownDenominationAmount(amount));
        BOOST_CHECK(CoinJoin::IsDfcnScaleDenomination(bit));
        // The wallet API answers as v23.0.0 does: not a denomination, an ordinary amount.
        BOOST_CHECK(!CoinJoin::IsDenominatedAmount(amount));
        BOOST_CHECK(!CoinJoin::IsDenominatedOrCollateralAmount(amount));
        BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(amount), -1 * (amount / COIN));
        const auto wallet_denoms{CoinJoin::GetStandardDenominations()};
        BOOST_CHECK(std::find(wallet_denoms.begin(), wallet_denoms.end(), amount) == wallet_denoms.end());
    }
    BOOST_CHECK_EQUAL(CoinJoin::DenominationToAmount(256), -1);
    BOOST_CHECK_EQUAL(CoinJoin::DenominationToAmount(32 | 64), -2);
    BOOST_CHECK(!CoinJoin::IsDfcnScaleDenomination(0));
    // The wallet's list, its smallest amount, the collateral and the pool maximum stay v23.0.0's.
    BOOST_CHECK_EQUAL(CoinJoin::GetStandardDenominations().size(), 5U);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallestDenomination(), 100001);
    BOOST_CHECK_EQUAL(CoinJoin::GetCollateralAmount(), 10000);
    BOOST_CHECK_EQUAL(CoinJoin::GetMaxPoolAmount(), 9 * CAmount{1000010000});
}

BOOST_AUTO_TEST_CASE(dfcn_scale_denominations_in_dstx_structure)
{
    const int participants = std::max(3, CoinJoin::GetMinPoolParticipants());
    const auto make = [&](CAmount first, CAmount rest) {
        CCoinJoinBroadcastTx dstx;
        CMutableTransaction mtx;
        for (int i = 0; i < participants; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(i == 0 ? first : rest, P2PKHScript(static_cast<uint8_t>(i)));
        }
        dstx.tx = MakeTransactionRef(mtx);
        dstx.m_protxHash = uint256::ONE;
        return dstx;
    };
    const CAmount old_amount{CoinJoin::GetSmallestDenomination()};
    const CAmount new_amount{CoinJoin::DenominationToAmount(32)};
    // Old amounts only: valid, nothing DFCN-scale in it.
    BOOST_CHECK(make(old_amount, old_amount).IsValidStructure());
    BOOST_CHECK(!make(old_amount, old_amount).HasDfcnScaleDenomination());
    // A new amount, alone or after an old one: structurally valid at every height, and DFCN-scale.
    BOOST_CHECK(make(new_amount, new_amount).IsValidStructure());
    BOOST_CHECK(make(new_amount, new_amount).HasDfcnScaleDenomination());
    BOOST_CHECK(make(old_amount, new_amount).IsValidStructure());
    BOOST_CHECK(make(old_amount, new_amount).HasDfcnScaleDenomination());
    // An amount of no table stays invalid.
    BOOST_CHECK(!make(old_amount, new_amount + 1).IsValidStructure());
}

// The masternode's gate for DFCN-scale sessions counts the next block's height. An unset H (the largest int) stays
// closed also at the one tip, INT_MAX - 1, where the next block's height would reach it.
BOOST_AUTO_TEST_CASE(dfcn_scale_session_gate_by_next_height)
{
    constexpr int unset{std::numeric_limits<int>::max()};
    BOOST_CHECK(!CoinJoin::DfcnScaleSessionsOpenAt(400, 398));
    BOOST_CHECK(CoinJoin::DfcnScaleSessionsOpenAt(400, 399));
    BOOST_CHECK(CoinJoin::DfcnScaleSessionsOpenAt(400, 400));
    BOOST_CHECK(!CoinJoin::DfcnScaleSessionsOpenAt(unset, 0));
    BOOST_CHECK(!CoinJoin::DfcnScaleSessionsOpenAt(unset, unset - 1));
    BOOST_CHECK(!CoinJoin::DfcnScaleSessionsOpenAt(unset, unset));
    // Just below the unset value the next height counts as for any H, without overflow.
    BOOST_CHECK(!CoinJoin::DfcnScaleSessionsOpenAt(unset - 1, unset - 3));
    BOOST_CHECK(CoinJoin::DfcnScaleSessionsOpenAt(unset - 1, unset - 2));
    // With H set, the tip INT_MAX: the next height is counted in 64 bits. An int addition would overflow here, which
    // a constant evaluation refuses, so this fails to compile rather than passing by chance.
    static_assert(CoinJoin::DfcnScaleSessionsOpenAt(400, std::numeric_limits<int>::max()));
    static_assert(CoinJoin::DfcnScaleSessionsOpenAt(unset - 1, unset));
    BOOST_CHECK(CoinJoin::DfcnScaleSessionsOpenAt(400, unset));
}

// The session amount check of a masternode (IsValidInOuts) reads the wire table: an entry of 100.001 DFCN coins
// is valid in a bit-5 session and is refused with ERR_DENOM, charging the collateral, in a bit-0 session.
class InOutsProbe : public CCoinJoinBaseSession
{
public:
    using CCoinJoinBaseSession::IsValidInOuts;
};

BOOST_AUTO_TEST_CASE(dfcn_scale_session_amount_check)
{
    const CAmount new_amount{CoinJoin::DenominationToAmount(1 << 5)};
    auto& chainstate{m_node.chainman->ActiveChainstate()};
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    {
        LOCK(cs_main);
        for (uint8_t i = 0; i < 2; ++i) {
            const COutPoint prevout{uint256S("c140000000000000000000000000000000000000000000000000000000000001"), i};
            chainstate.CoinsTip().AddCoin(prevout,
                                          Coin{CTxOut{new_amount, P2PKHScript(static_cast<uint8_t>(0x10 + i))}, 1,
                                               /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/false},
                                          /*possible_overwrite=*/false);
            vin.emplace_back(prevout);
            vout.emplace_back(new_amount, P2PKHScript(static_cast<uint8_t>(0x20 + i)));
        }
    }
    InOutsProbe session;
    PoolMessage message{MSG_NOERR};
    bool consume{true};

    session.nSessionDenom = 1 << 5;
    BOOST_CHECK(session.IsValidInOuts(chainstate, *m_node.llmq_ctx->isman, *m_node.mempool, vin, vout, message, &consume));
    BOOST_CHECK_EQUAL(message, MSG_NOERR);
    BOOST_CHECK(!consume);

    session.nSessionDenom = 1 << 0;
    BOOST_CHECK(!session.IsValidInOuts(chainstate, *m_node.llmq_ctx->isman, *m_node.mempool, vin, vout, message, &consume));
    BOOST_CHECK_EQUAL(message, ERR_DENOM);
    BOOST_CHECK(consume);
}

BOOST_AUTO_TEST_SUITE_END()
