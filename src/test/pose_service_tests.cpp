// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <evo/pose_service.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <limits>

BOOST_FIXTURE_TEST_SUITE(pose_service_tests, BasicTestingSetup)

namespace {
//! The release height: the one place this file names it. It is V23_MAINNET_ACTIVATION_HEIGHT in
//! chainparams.cpp, and moving the release height moves both.
constexpr int RELEASE_H = 144888;
constexpr int DSL_OFFSET = 24 * 24; // V23_DSL_ACTIVATION_OFFSET, chainparams.cpp: one day of epochs

CPoSeServiceCommitment MakeCommitment()
{
    CPoSeServiceCommitment c;
    c.nEpoch = 42;
    c.epochBlockHash = uint256::ONE;
    c.llmqType = Consensus::LLMQType::LLMQ_50_60;
    c.quorumHash = uint256::TWO;
    c.missed = {true, false, false, true, false, true, false};
    // a superset of missed, with two indices the epoch reached no verdict on
    c.observed = {true, false, true, true, true, true, false};
    return c;
}
} // namespace

BOOST_AUTO_TEST_CASE(commitment_roundtrips)
{
    const CPoSeServiceCommitment c = MakeCommitment();
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << c;
    CPoSeServiceCommitment back;
    ss >> back;

    BOOST_CHECK_EQUAL(back.nVersion, c.nVersion);
    BOOST_CHECK_EQUAL(back.nEpoch, c.nEpoch);
    BOOST_CHECK(back.epochBlockHash == c.epochBlockHash);
    BOOST_CHECK(back.llmqType == c.llmqType);
    BOOST_CHECK(back.quorumHash == c.quorumHash);
    BOOST_CHECK(back.missed == c.missed);
    BOOST_CHECK(back.observed == c.observed);
    BOOST_CHECK_EQUAL(back.CountMissed(), 3);
    BOOST_CHECK_EQUAL(back.CountUnobserved(), 2);

    // Version 1 carries no observed field at all: the bytes are exactly the
    // pre-version-2 bytes whatever the in-memory vector holds, so the history
    // on chain deserializes and hashes as it always did.
    CPoSeServiceCommitment legacy = c;
    legacy.nVersion = CPoSeServiceCommitment::LEGACY_VERSION;
    CPoSeServiceCommitment legacy_without = legacy;
    legacy_without.observed.clear();
    CDataStream l1(SER_NETWORK, PROTOCOL_VERSION), l2(SER_NETWORK, PROTOCOL_VERSION);
    l1 << legacy;
    l2 << legacy_without;
    BOOST_CHECK(std::vector<std::byte>(l1.begin(), l1.end()) == std::vector<std::byte>(l2.begin(), l2.end()));
    CPoSeServiceCommitment legacy_back;
    l1 >> legacy_back;
    BOOST_CHECK_EQUAL(legacy_back.nVersion, CPoSeServiceCommitment::LEGACY_VERSION);
    BOOST_CHECK(legacy_back.observed.empty());
    BOOST_CHECK(legacy_back.missed == c.missed);
    // and a version-1 commitment observes everyone, by definition
    for (size_t i = 0; i < legacy_back.missed.size(); ++i) BOOST_CHECK(legacy_back.IsObserved(i));
    BOOST_CHECK_EQUAL(legacy_back.CountUnobserved(), 0);

    // and through the payload wrapper
    CPoSeServiceCommitmentTxPayload p;
    p.commitment = c;
    CDataStream ps(SER_NETWORK, PROTOCOL_VERSION);
    ps << p;
    CPoSeServiceCommitmentTxPayload pback;
    ps >> pback;
    BOOST_CHECK(pback.commitment.missed == c.missed);
    BOOST_CHECK_EQUAL(pback.commitment.nEpoch, c.nEpoch);
}

// The signing-session id binds a recovered threshold sig to its epoch: two
// epochs must never share an id (that would let a signature be replayed across
// epochs), and the same epoch must always produce the same id.
BOOST_AUTO_TEST_CASE(request_id_is_per_epoch)
{
    CPoSeServiceCommitment a = MakeCommitment();
    CPoSeServiceCommitment b = MakeCommitment();
    BOOST_CHECK(a.GetRequestId() == b.GetRequestId());

    b.nEpoch = a.nEpoch + 1;
    BOOST_CHECK(a.GetRequestId() != b.GetRequestId());

    // it also depends on the epoch base, so a reorg that rebases the same epoch
    // opens a distinct signing session instead of colliding with the old one
    CPoSeServiceCommitment d = MakeCommitment();
    d.epochBlockHash = uint256::TWO;
    BOOST_CHECK(a.GetRequestId() != d.GetRequestId());

    // but not on the bitfield -- the same epoch and base sign the same session
    CPoSeServiceCommitment c = MakeCommitment();
    c.missed = {false, false};
    BOOST_CHECK(a.GetRequestId() == c.GetRequestId());
}

// The value that picks an epoch's attesting quorum. The signer selects with it
// and the block rule re-derives the selection to check the commitment came from
// that quorum and no other, so the two must be the same expression: a drift
// would show up only as a commitment the network signs and then refuses.
BOOST_AUTO_TEST_CASE(quorum_selection_hash_is_per_epoch_only)
{
    BOOST_CHECK(ServiceCommitmentQuorumSelectionHash(7) == ServiceCommitmentQuorumSelectionHash(7));
    BOOST_CHECK(ServiceCommitmentQuorumSelectionHash(7) != ServiceCommitmentQuorumSelectionHash(8));

    // It names the epoch and nothing else -- deliberately not the epoch base,
    // which joins the request id instead. A reorg that rebases an epoch
    // therefore opens a fresh signing session against the same quorum, rather
    // than moving the epoch to a different one.
    CPoSeServiceCommitment a = MakeCommitment();
    CPoSeServiceCommitment b = MakeCommitment();
    b.epochBlockHash = uint256::TWO;
    BOOST_CHECK(a.GetRequestId() != b.GetRequestId());
    BOOST_CHECK(ServiceCommitmentQuorumSelectionHash(a.nEpoch) ==
                ServiceCommitmentQuorumSelectionHash(b.nEpoch));
}

// The layer's observing half is scheduled on mainnet one day of epochs after
// the release height, and stays dormant on testnet and regtest. Its enforcing
// half rides no release yet: the unreachable maximum on all three. The epoch
// length must be the Q60 DKG interval, and the start must sit on that grid.
BOOST_AUTO_TEST_CASE(activation_heights_are_pinned)
{
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(ArgsManager{}, chain);
        const auto& c = params->GetConsensus();
        const int expected_start = chain == CBaseChainParams::MAIN ? RELEASE_H + DSL_OFFSET : std::numeric_limits<int>::max();
        BOOST_CHECK_EQUAL(c.nDSLActivationHeight, expected_start);
        if (chain == CBaseChainParams::MAIN) BOOST_CHECK_EQUAL(c.nDSLActivationHeight % c.nDSLEpochInterval, 0);
        BOOST_CHECK_EQUAL(c.nDSLEnforcementHeight, std::numeric_limits<int>::max());
        BOOST_CHECK_EQUAL(c.nDSLEpochInterval, 24);
        // None of these three carries a version-1 commitment, so version 2 is
        // required from the first one: whenever the layer is activated, it
        // starts on the format that can say "no verdict".
        BOOST_CHECK_EQUAL(c.nDSLCommitmentV2Height, 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
