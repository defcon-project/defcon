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
CPoSeServiceCommitment MakeCommitment()
{
    CPoSeServiceCommitment c;
    c.nEpoch = 42;
    c.epochBlockHash = uint256::ONE;
    c.llmqType = Consensus::LLMQType::LLMQ_50_60;
    c.quorumHash = uint256::TWO;
    c.missed = {true, false, false, true, false, true, false};
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
    BOOST_CHECK_EQUAL(back.CountMissed(), 3);

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

// DSL ships dormant: its activation and enforcement heights must be the
// unreachable maximum on every network until a coordinated release sets them,
// and the epoch length must be the Q60 DKG interval.
BOOST_AUTO_TEST_CASE(activation_is_pinned_dormant)
{
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(ArgsManager{}, chain);
        const auto& c = params->GetConsensus();
        BOOST_CHECK_EQUAL(c.nDSLActivationHeight, std::numeric_limits<int>::max());
        BOOST_CHECK_EQUAL(c.nDSLEnforcementHeight, std::numeric_limits<int>::max());
        BOOST_CHECK_EQUAL(c.nDSLEpochInterval, 24);
    }
}

BOOST_AUTO_TEST_SUITE_END()
