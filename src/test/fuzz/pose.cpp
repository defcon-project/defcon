// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service.h>
#include <evo/pose_service_sentinels.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <version.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <ios>
#include <vector>

namespace {

template <typename T>
void AssertCanonicalRoundTrip(const T& value)
{
    CDataStream encoded{SER_NETWORK, PROTOCOL_VERSION};
    encoded << value;

    T decoded;
    CDataStream round_trip{Span<const std::byte>{encoded.data(), encoded.size()}, SER_NETWORK, PROTOCOL_VERSION};
    round_trip >> decoded;

    CDataStream reencoded{SER_NETWORK, PROTOCOL_VERSION};
    reencoded << decoded;
    assert(encoded.size() == reencoded.size());
    assert(std::equal(encoded.begin(), encoded.end(), reencoded.begin()));
}

template <typename T>
void DeserializeAndCheckCanonicalRoundTrip(FuzzBufferType buffer)
{
    try {
        CDataStream input{buffer, SER_NETWORK, PROTOCOL_VERSION};
        T value;
        input >> value;
        AssertCanonicalRoundTrip(value);
    } catch (const std::ios_base::failure&) {
        // Arbitrary input is expected to fail deserialization.
    }
}

void DeserializeCommitmentVersionAndCheckCanonicalRoundTrip(FuzzBufferType buffer, uint16_t version)
{
    try {
        std::vector<uint8_t> bytes{static_cast<uint8_t>(version), static_cast<uint8_t>(version >> 8)};
        bytes.insert(bytes.end(), buffer.begin(), buffer.end());

        CDataStream input{bytes, SER_NETWORK, PROTOCOL_VERSION};
        CPoSeServiceCommitment value;
        input >> value;
        assert(value.nVersion == version);
        AssertCanonicalRoundTrip(value);
    } catch (const std::ios_base::failure&) {
        // Arbitrary input is expected to fail deserialization.
    }
}

void CommitmentVersionRoundTrip(FuzzBufferType buffer, uint16_t version)
{
    CPoSeServiceCommitment value;
    value.nVersion = version;
    value.nEpoch = static_cast<uint32_t>(buffer.size());

    // Bound the generated bitsets while keeping the v2 invariant that every
    // missed target was observed. Version 1 has no observed field on the wire.
    const size_t bit_count = std::min<size_t>(buffer.size(), 256);
    for (size_t i = 0; i < bit_count; ++i) {
        const bool observed = (buffer[i] & 1U) != 0;
        const bool missed = (buffer[i] & 2U) != 0;
        value.missed.push_back(version == CPoSeServiceCommitment::LEGACY_VERSION ? missed : observed && missed);
        if (version >= CPoSeServiceCommitment::OBSERVED_VERSION) {
            value.observed.push_back(observed);
        }
    }

    AssertCanonicalRoundTrip(value);
}

void CheckReportPeekMatchesDeserialize(Span<const unsigned char> bytes, bool expect_valid_signature = false)
{
    if (bytes.size() != dsl::CPoSeServiceReport::WIRE_SIZE) return;

    const auto wire = dsl::PeekReportWire(bytes);
    assert(wire.has_value());

    dsl::CPoSeServiceReport decoded;
    CDataStream input{bytes, SER_NETWORK, PROTOCOL_VERSION};
    try {
        input >> decoded;
    } catch (const std::ios_base::failure&) {
        // Arbitrary signature bytes are usually not a valid serialized report.
        return;
    }
    assert(input.empty());

    assert(wire->nEpoch == decoded.nEpoch);
    assert(wire->targetProTxHash == decoded.targetProTxHash);
    assert(wire->sentinelProTxHash == decoded.sentinelProTxHash);
    assert(wire->status == decoded.status);
    assert(wire->sig.data() == bytes.data() + bytes.size() - static_cast<size_t>(BLS_CURVE_SIG_SIZE));
    if (!decoded.sig.IsValid()) {
        // Peek intentionally retains malformed BLS bytes; deserialization resets invalid signatures.
        assert(!expect_valid_signature);
        return;
    }
    const auto signature = decoded.sig.ToBytes(/*specificLegacyScheme=*/false);
    assert(wire->sig.size() == signature.size());
    assert(std::equal(signature.begin(), signature.end(), wire->sig.begin()));
}

} // namespace

FUZZ_TARGET(pose_service_challenge_deserialize) {
    DeserializeAndCheckCanonicalRoundTrip<dsl::CPoSeServiceChallenge>(buffer);
}
FUZZ_TARGET(pose_service_response_deserialize) {
    DeserializeAndCheckCanonicalRoundTrip<dsl::CPoSeServiceResponse>(buffer);
}
FUZZ_TARGET(pose_service_report_deserialize) {
    DeserializeAndCheckCanonicalRoundTrip<dsl::CPoSeServiceReport>(buffer);
}
FUZZ_TARGET(pose_service_commitment_deserialize) {
    DeserializeAndCheckCanonicalRoundTrip<CPoSeServiceCommitment>(buffer);
}
FUZZ_TARGET(pose_service_commitment_v1_deserialize) {
    DeserializeCommitmentVersionAndCheckCanonicalRoundTrip(buffer, CPoSeServiceCommitment::LEGACY_VERSION);
}
FUZZ_TARGET(pose_service_commitment_v2_deserialize) {
    DeserializeCommitmentVersionAndCheckCanonicalRoundTrip(buffer, CPoSeServiceCommitment::OBSERVED_VERSION);
}
FUZZ_TARGET(pose_service_commitment_v1_roundtrip) {
    CommitmentVersionRoundTrip(buffer, CPoSeServiceCommitment::LEGACY_VERSION);
}
FUZZ_TARGET(pose_service_commitment_v2_roundtrip) {
    CommitmentVersionRoundTrip(buffer, CPoSeServiceCommitment::OBSERVED_VERSION);
}

FUZZ_TARGET(pose_report_wire_differential) {
    // Keep malformed wire bytes in the fuzzer's input domain.
    CheckReportPeekMatchesDeserialize(Span<const unsigned char>{buffer.data(), buffer.size()});

    // Also generate a valid signed report from fuzzed fields. This guarantees
    // the differential oracle exercises successful full deserialization.
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    dsl::CPoSeServiceReport report;
    report.nEpoch = provider.ConsumeIntegral<uint32_t>();
    const auto target_bytes = provider.ConsumeBytes<unsigned char>(uint256::size());
    std::copy(target_bytes.begin(), target_bytes.end(), report.targetProTxHash.begin());
    const auto sentinel_bytes = provider.ConsumeBytes<unsigned char>(uint256::size());
    std::copy(sentinel_bytes.begin(), sentinel_bytes.end(), report.sentinelProTxHash.begin());
    report.status = provider.ConsumeIntegral<uint8_t>();

    static const CBLSSecretKey signing_key = [] {
        CBLSSecretKey key;
        key.MakeNewKey();
        return key;
    }();
    report.Sign(signing_key, uint256{});

    CDataStream encoded{SER_NETWORK, PROTOCOL_VERSION};
    encoded << report;
    const std::vector<unsigned char> bytes{UCharCast(encoded.data()), UCharCast(encoded.data()) + encoded.size()};
    CheckReportPeekMatchesDeserialize(MakeUCharSpan(bytes), /*expect_valid_signature=*/true);
}
