// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service.h>
#include <evo/pose_service_sentinels.h>
#include <streams.h>
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
