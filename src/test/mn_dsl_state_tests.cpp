// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <memory>

BOOST_FIXTURE_TEST_SUITE(mn_dsl_state_tests, BasicTestingSetup)

// A masternode state written before the service-PoSe fields existed (the
// MN_COMPUTE_FORMAT era) must read back byte-exactly through the frozen
// no-dsl format, with the DSL fields at their defaults and the compute
// descriptor preserved. This is the contract the evodb migration relies on.
BOOST_AUTO_TEST_CASE(pre_dsl_state_reads_back)
{
    CDeterministicMNState_no_dsl_format old_state;
    old_state.nRegisteredHeight = 123;
    old_state.nLastPaidHeight = 456;
    old_state.nConsecutivePayments = 2;
    CKeyID owner;
    std::memset(owner.begin(), 0x22, owner.size());
    old_state.keyIDOwner = owner;
    old_state.computeDescriptor.vchOracleKey = ParseHex("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    old_state.computeDescriptor.certExpiryHeight = 5000;

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << uint256::ONE;            // proTxHash
    uint64_t internal_id{7};
    ss << VARINT(internal_id);
    ss << COutPoint(uint256::TWO, 1);
    ss << uint16_t{0};             // nOperatorReward
    ss << old_state;
    ss << MnType::Compute;

    CDeterministicMN dmn(deserialize, ss, CDeterministicMN::MN_COMPUTE_FORMAT);
    BOOST_CHECK(dmn.proTxHash == uint256::ONE);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nRegisteredHeight, 123);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nLastPaidHeight, 456);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nConsecutivePayments, 2);
    BOOST_CHECK(dmn.pdmnState->keyIDOwner == owner);
    // the compute descriptor survives the format conversion...
    BOOST_CHECK_EQUAL(dmn.pdmnState->computeDescriptor.certExpiryHeight, 5000);
    // ...and the new service-PoSe fields default.
    BOOST_CHECK_EQUAL(dmn.pdmnState->nMissedEpochs, 0u);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nLastServiceEpoch, 0u);
    BOOST_CHECK_EQUAL(dmn.pdmnState->fRewardSuspended, false);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nDSLBanHeight, -1);
}

// A state carrying service-PoSe fields round-trips at the current format.
BOOST_AUTO_TEST_CASE(dsl_state_roundtrips)
{
    auto dmn = std::make_shared<CDeterministicMN>(/*internalId=*/9);
    dmn->proTxHash = uint256::ONE;
    dmn->collateralOutpoint = COutPoint(uint256::TWO, 3);
    auto st = std::make_shared<CDeterministicMNState>();
    st->nRegisteredHeight = 10;
    st->nMissedEpochs = 4;
    st->nLastServiceEpoch = 77;
    st->fRewardSuspended = true;
    st->nDSLBanHeight = 1234;
    dmn->pdmnState = st;

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << *dmn;
    CDeterministicMN back(deserialize, ss, CDeterministicMN::MN_CURRENT_FORMAT);

    BOOST_CHECK_EQUAL(back.pdmnState->nMissedEpochs, 4u);
    BOOST_CHECK_EQUAL(back.pdmnState->nLastServiceEpoch, 77u);
    BOOST_CHECK_EQUAL(back.pdmnState->fRewardSuspended, true);
    BOOST_CHECK_EQUAL(back.pdmnState->nDSLBanHeight, 1234);
}

// IsBanned() must cover a DSL ban held on its own field: the two ban origins
// are independent, so a service ban excludes the masternode even with no DKG
// PoSe ban.
BOOST_AUTO_TEST_CASE(is_banned_covers_dsl_ban)
{
    CDeterministicMNState st;
    BOOST_CHECK(!st.IsBanned());
    st.nDSLBanHeight = 500;
    BOOST_CHECK(st.IsBanned());
    // a DKG revive does not clear a DSL ban
    st.Revive(600);
    BOOST_CHECK(st.IsBanned());
    st.nDSLBanHeight = -1;
    BOOST_CHECK(!st.IsBanned());
}

BOOST_AUTO_TEST_SUITE_END()
