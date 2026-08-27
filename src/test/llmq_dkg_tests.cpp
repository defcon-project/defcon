// Copyright (c) 2022-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/dkgsession.h>
#include <streams.h>
#include <protocol.h>
#include <llmq/dkgsessionhandler.h>
#include <netmessagemaker.h>
#include <llmq/params.h>
#include <util/irange.h>
#include <util/underlying.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(llmq_dkg_tests)

BOOST_AUTO_TEST_CASE(llmq_dkgerror)
{
    using namespace llmq;
    for (auto i : irange::range(ToUnderlying(llmq::DKGError::type::_COUNT))) {
        BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type(i)) == 0.0);
        SetSimulatedDKGErrorRate(llmq::DKGError::type(i), 1.0);
        BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type(i)) == 1.0);
    }
    BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type::_COUNT) == 0.0);
    SetSimulatedDKGErrorRate(llmq::DKGError::type::_COUNT, 1.0);
    BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type::_COUNT) == 0.0);
}



namespace {
std::shared_ptr<CDataStream> MakeDKGMessage()
{
    return std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION);
}

uint256 MakeTestHash(uint8_t value)
{
    uint256 hash;
    hash.begin()[0] = value;
    return hash;
}
} // namespace

// dash#7583, adapted: our CDKGPendingMessages constructor also takes the inv
// type, and the hash-and-erase wrapper is separate so these tests can push
// with an explicit hash and no PeerManager.

BOOST_AUTO_TEST_CASE(pending_messages_own_messages_share_quota_path)
{
    using namespace llmq;

    const uint256 own_protx = MakeTestHash(0xee);

    // Own messages (from=-1) are enqueued under this node's proTxHash and
    // charged like any other sender's.
    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2, /*_invType=*/0};
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(2));
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(3));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(3).size(), 2U);
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_survives_reconnect)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2, /*_invType=*/0};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(1)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));

    // Reconnecting mints a fresh NodeId but keeps the proTxHash, so the quota
    // is already spent.
    pending.PushPendingMessage(/*from=*/2, protx_a, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/3, protx_a, MakeDKGMessage(), MakeTestHash(4));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(4)));

    // Draining frees queue slots but does not refund the per-proTx quota.
    BOOST_CHECK_EQUAL(pending.PopPendingMessages(5).size(), 2U);
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A new round resets everything.
    pending.Clear();
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_is_per_protx)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);
    const uint256 protx_b = MakeTestHash(0xb1);

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2, /*_invType=*/0};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));

    // Duplicates are rejected before charging the quota.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(2));

    // One proTx's spent quota has no effect on another's.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(4));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(4)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A quota-dropped hash is not marked seen, so a sender with remaining
    // budget can still deliver it.
    const uint256 protx_c = MakeTestHash(0xc1);
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(6).size(), 5U);
}

using namespace llmq;

namespace {

// Small synthetic profile so the wire-structure walks are exercised against
// exact bounds: 5 members, 3 needed to form, threshold 2.
Consensus::LLMQParams WireTestParams()
{
    return Consensus::LLMQParams{
        .type = Consensus::LLMQType::LLMQ_TEST,
        .name = "wire_test",
        .useRotation = false,
        .size = 5,
        .minSize = 3,
        .threshold = 2,
        .dkgInterval = 24,
        .dkgPhaseBlocks = 2,
        .dkgMiningWindowStart = 10,
        .dkgMiningWindowEnd = 18,
        .dkgBadVotesThreshold = 4,
        .signingActiveQuorumCount = 2,
        .keepOldConnections = 3,
        .keepOldKeys = 4,
        .recoveryMembers = 3,
    };
}

void AppendZeros(CDataStream& s, size_t n)
{
    const std::vector<uint8_t> zeros(n, 0);
    s.write(AsBytes(Span{zeros}));
}

CDataStream MakeDKGStream()
{
    CDataStream s{SER_NETWORK, PROTOCOL_VERSION};
    AppendZeros(s, 1 + 32 + 32); // llmqType + quorumHash + proTxHash
    return s;
}

CDataStream BuildContribPayload(size_t vvec_count, size_t blob_count, size_t blob_len = 32)
{
    CDataStream s = MakeDKGStream();
    WriteCompactSize(s, vvec_count);
    AppendZeros(s, vvec_count * CBLSPublicKey::SerSize);
    AppendZeros(s, CBLSPublicKey::SerSize + 32); // IES ephemeralPubKey + ivSeed
    WriteCompactSize(s, blob_count);
    for (size_t i = 0; i < blob_count; ++i) {
        WriteCompactSize(s, blob_len);
        AppendZeros(s, blob_len);
    }
    AppendZeros(s, CBLSSignature::SerSize);
    return s;
}

void AppendDynBitset(CDataStream& s, size_t bit_count)
{
    WriteCompactSize(s, bit_count);
    AppendZeros(s, (bit_count + 7) / 8);
}

CDataStream BuildComplaintPayload(size_t bad_bits, size_t complain_bits)
{
    CDataStream s = MakeDKGStream();
    AppendDynBitset(s, bad_bits);
    AppendDynBitset(s, complain_bits);
    AppendZeros(s, CBLSSignature::SerSize);
    return s;
}

CDataStream BuildJustificationPayload(size_t contribution_count)
{
    CDataStream s = MakeDKGStream();
    WriteCompactSize(s, contribution_count);
    AppendZeros(s, contribution_count * (4 + CBLSSecretKey::SerSize));
    AppendZeros(s, CBLSSignature::SerSize);
    return s;
}

CDataStream BuildCommitmentPayload(size_t valid_bits)
{
    CDataStream s = MakeDKGStream();
    AppendDynBitset(s, valid_bits);
    AppendZeros(s, CBLSPublicKey::SerSize + 32 + 2 * CBLSSignature::SerSize);
    return s;
}

} // namespace

// Adapted from upstream's NetDKG intake layer: the framing walk must accept
// exactly the payloads the typed deserializer would, and reject everything
// whose declared counts violate the quorum params -- before any BLS decode.
BOOST_AUTO_TEST_CASE(dkg_wire_structure_contribution)
{
    const auto params = WireTestParams();
    const size_t size = static_cast<size_t>(params.size);
    const size_t threshold = static_cast<size_t>(params.threshold);

    const auto ok = BuildContribPayload(threshold, size);
    BOOST_CHECK(CheckDKGMessageWireStructure(NetMsgType::QCONTRIB, ok, params));
    // The size cap admits every well-formed payload of this profile.
    BOOST_CHECK_LE(ok.size(), MaxDKGMessageSize(NetMsgType::QCONTRIB, params));

    // vvec must declare exactly threshold public keys.
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCONTRIB, BuildContribPayload(threshold + 1, size), params));
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCONTRIB, BuildContribPayload(threshold - 1, size), params));

    // More encrypted blobs than members cannot be well-formed.
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCONTRIB, BuildContribPayload(threshold, size + 1), params));

    // Trailing bytes and truncation both reject.
    {
        CDataStream trailing = BuildContribPayload(threshold, size);
        AppendZeros(trailing, 1);
        BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCONTRIB, trailing, params));
    }
    {
        CDataStream full = BuildContribPayload(threshold, size);
        CDataStream truncated{SER_NETWORK, PROTOCOL_VERSION};
        truncated.write(AsBytes(Span{full.data(), full.size() - 1}));
        BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCONTRIB, truncated, params));
    }
}

BOOST_AUTO_TEST_CASE(dkg_wire_structure_complaint)
{
    const auto params = WireTestParams();
    const size_t size = static_cast<size_t>(params.size);

    BOOST_CHECK(CheckDKGMessageWireStructure(NetMsgType::QCOMPLAINT, BuildComplaintPayload(size, size), params));

    // A bitset wider than the quorum, or two bitsets of different widths, reject.
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCOMPLAINT, BuildComplaintPayload(size + 1, size + 1), params));
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QCOMPLAINT, BuildComplaintPayload(size, size - 1), params));
}

BOOST_AUTO_TEST_CASE(dkg_wire_structure_justification_and_commitment)
{
    const auto params = WireTestParams();
    const size_t size = static_cast<size_t>(params.size);

    BOOST_CHECK(CheckDKGMessageWireStructure(NetMsgType::QJUSTIFICATION, BuildJustificationPayload(size), params));
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QJUSTIFICATION, BuildJustificationPayload(size + 1), params));

    BOOST_CHECK(CheckDKGMessageWireStructure(NetMsgType::QPCOMMITMENT, BuildCommitmentPayload(size), params));
    BOOST_CHECK(!CheckDKGMessageWireStructure(NetMsgType::QPCOMMITMENT, BuildCommitmentPayload(size + 1), params));

    // Unknown message types never pass the walk.
    BOOST_CHECK(!CheckDKGMessageWireStructure("qunknown", BuildJustificationPayload(size), params));
}

BOOST_AUTO_TEST_CASE(dkg_max_message_size_bounds)
{
    const auto params = WireTestParams();
    constexpr size_t HARD_CEILING = size_t{1} << 20;

    // Every known type gets a params-derived cap below the ceiling; unknown
    // types fall back to the ceiling rather than the transport cap.
    for (const auto* msg_type : {NetMsgType::QCONTRIB, NetMsgType::QCOMPLAINT,
                                 NetMsgType::QJUSTIFICATION, NetMsgType::QPCOMMITMENT}) {
        BOOST_CHECK_LT(MaxDKGMessageSize(msg_type, params), HARD_CEILING);
    }
    BOOST_CHECK_EQUAL(MaxDKGMessageSize("qunknown", params), HARD_CEILING);

    // A profile large enough to overflow the ceiling is clamped to it.
    auto huge = WireTestParams();
    huge.size = 10000;
    huge.threshold = 8000;
    BOOST_CHECK_EQUAL(MaxDKGMessageSize(NetMsgType::QCONTRIB, huge), HARD_CEILING);
}

BOOST_AUTO_TEST_SUITE_END()
