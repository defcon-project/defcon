// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <llmq/quorums.h>
#include <uint256.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;

// dash#7519, adapted: the tracking state lives in file statics shared across
// the whole test binary, so every case advances mock time past the expiry
// bias and runs CleanupExpiredDataRequests before handing the map back.

namespace {

CQuorumDataRequestKey MakeInboundKey(const uint256& requester, const uint256& quorum_hash)
{
    return {requester, /*m_we_requested=*/false, quorum_hash, Consensus::LLMQType::LLMQ_DEVNET};
}

CQuorumDataRequest MakeRequest(const uint256& quorum_hash)
{
    return {Consensus::LLMQType::LLMQ_DEVNET, quorum_hash, CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR};
}

void DrainTrackingState(int64_t from)
{
    SetMockTime(from + CQuorumDataRequest::EXPIRATION_TIMEOUT + CQuorumDataRequest::EXPIRATION_BIAS + 1);
    CQuorumManager::CleanupExpiredDataRequests();
    SetMockTime(0);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_qgetdata_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(qgetdata_request_tracking_is_bounded_per_requester)
{
    constexpr int64_t kNow{100000};
    SetMockTime(kNow);
    const uint256 requester{uint256S("0x77")};

    for (size_t i = 0; i < MAX_INBOUND_DATA_REQUESTS_PER_REQUESTER; ++i) {
        const uint256 hash{ArithToUint256(arith_uint256{i + 1})};
        BOOST_CHECK(CQuorumManager::RegisterDataRequest(MakeInboundKey(requester, hash), MakeRequest(hash),
                                                        /*add_expiry_bias=*/false) ==
                    DataRequestRegistration::Accepted);
    }

    const uint256 over_hash{uint256S("0xdead")};
    const auto over_key = MakeInboundKey(requester, over_hash);
    BOOST_CHECK(CQuorumManager::RegisterDataRequest(over_key, MakeRequest(over_hash), /*add_expiry_bias=*/false) ==
                DataRequestRegistration::RequesterLimitExceeded);
    // The over-limit request was dropped, not tracked: were it in the map, the
    // repeat would answer RateLimited instead of hitting the budget again.
    BOOST_CHECK(CQuorumManager::RegisterDataRequest(over_key, MakeRequest(over_hash), /*add_expiry_bias=*/false) ==
                DataRequestRegistration::RequesterLimitExceeded);

    // Timer-driven cleanup recovers capacity even when no block arrives.
    SetMockTime(kNow + CQuorumDataRequest::EXPIRATION_TIMEOUT + CQuorumDataRequest::EXPIRATION_BIAS + 1);
    CQuorumManager::CleanupExpiredDataRequests();
    BOOST_CHECK(CQuorumManager::RegisterDataRequest(over_key, MakeRequest(over_hash), /*add_expiry_bias=*/false) ==
                DataRequestRegistration::Accepted);

    DrainTrackingState(GetTime());
}

BOOST_AUTO_TEST_CASE(qgetdata_null_identity_budget_is_not_attributed)
{
    constexpr int64_t kNow{200000};
    SetMockTime(kNow);
    // All unauthenticated qwatch peers share the null identity, so exhausting
    // that shared budget must read as capacity, not as one peer misbehaving.
    const uint256 requester{};

    for (size_t i = 0; i < MAX_INBOUND_DATA_REQUESTS_PER_REQUESTER; ++i) {
        const uint256 hash{ArithToUint256(arith_uint256{i + 1})};
        BOOST_CHECK(CQuorumManager::RegisterDataRequest(MakeInboundKey(requester, hash), MakeRequest(hash),
                                                        /*add_expiry_bias=*/false) ==
                    DataRequestRegistration::Accepted);
    }

    const uint256 over_hash{uint256S("0xdead")};
    BOOST_CHECK(CQuorumManager::RegisterDataRequest(MakeInboundKey(requester, over_hash), MakeRequest(over_hash),
                                                    /*add_expiry_bias=*/false) ==
                DataRequestRegistration::CapacityExhausted);

    DrainTrackingState(kNow);
}

BOOST_AUTO_TEST_CASE(qgetdata_global_capacity_does_not_limit_outbound_requests)
{
    constexpr int64_t kNow{300000};
    SetMockTime(kNow);

    for (size_t i = 0; i < MAX_INBOUND_DATA_REQUESTS; ++i) {
        const uint256 requester{ArithToUint256(arith_uint256{i / MAX_INBOUND_DATA_REQUESTS_PER_REQUESTER + 1})};
        const uint256 hash{ArithToUint256(arith_uint256{i + 1})};
        BOOST_REQUIRE(CQuorumManager::RegisterDataRequest(MakeInboundKey(requester, hash), MakeRequest(hash),
                                                          /*add_expiry_bias=*/false) ==
                      DataRequestRegistration::Accepted);
    }

    const uint256 extra_requester{uint256S("0xbeef")};
    const uint256 extra_hash{uint256S("0xfeed")};
    BOOST_CHECK(CQuorumManager::RegisterDataRequest(MakeInboundKey(extra_requester, extra_hash),
                                                    MakeRequest(extra_hash), /*add_expiry_bias=*/false) ==
                DataRequestRegistration::CapacityExhausted);
    // An outbound request we initiate ourselves consumes no inbound budget.
    BOOST_CHECK(CQuorumManager::RegisterDataRequest({extra_requester, /*m_we_requested=*/true, extra_hash,
                                                     Consensus::LLMQType::LLMQ_DEVNET},
                                                    MakeRequest(extra_hash), /*add_expiry_bias=*/false) ==
                DataRequestRegistration::Accepted);

    DrainTrackingState(kNow);
}

BOOST_AUTO_TEST_SUITE_END()
