// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/compute_descriptor.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <netbase.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <memory>

BOOST_FIXTURE_TEST_SUITE(compute_descriptor_tests, BasicTestingSetup)

namespace {
//! The secp256k1 generator point: a compressed key that is always fully valid.
const std::vector<unsigned char> VALID_ORACLE_KEY = ParseHex("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");

CComputeServiceDescriptor ValidDescriptor()
{
    CComputeServiceDescriptor d;
    d.vchOracleKey = VALID_ORACLE_KEY;
    d.endpoint = "oracle.example:443";
    d.certHash = uint256::ONE;
    d.certExpiryHeight = 5000;
    d.capabilityBits = 0x3;
    return d;
}

CProUpServTx ComputeProUpServ(const CComputeServiceDescriptor& d)
{
    CProUpServTx proTx;
    proTx.nVersion = CProUpServTx::BASIC_BLS_VERSION;
    proTx.nType = MnType::Compute;
    proTx.addr = LookupNumeric("1.1.1.1", 1000);
    proTx.computeDescriptor = d;
    return proTx;
}

void ExpectRejected(const CComputeServiceDescriptor& d, const std::string& reason)
{
    TxValidationState state;
    BOOST_CHECK(!ComputeProUpServ(d).IsTriviallyValid(/*is_basic_scheme_active=*/true, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), reason);
}
} // namespace

BOOST_AUTO_TEST_CASE(descriptor_roundtrips)
{
    const CComputeServiceDescriptor d = ValidDescriptor();
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << d;
    CComputeServiceDescriptor d2;
    ss >> d2;
    BOOST_CHECK(d == d2);
}

BOOST_AUTO_TEST_CASE(cert_validity_is_height_arithmetic)
{
    CComputeServiceDescriptor d = ValidDescriptor();
    BOOST_CHECK(d.IsCertValidAt(0));
    BOOST_CHECK(d.IsCertValidAt(4999));
    // the expiry height itself is the first invalid one
    BOOST_CHECK(!d.IsCertValidAt(5000));
    d.certHash = uint256();
    d.certExpiryHeight = 0;
    // no certificate is never a valid certificate
    BOOST_CHECK(!d.IsCertValidAt(0));
}

// Trivial validation owns the descriptor's shape. Every rejection is proven
// against a baseline that passes, so a check that could never fire would show
// up here as a failure instead of as silent acceptance.
BOOST_AUTO_TEST_CASE(descriptor_shape_is_trivially_validated)
{
    {
        TxValidationState state;
        BOOST_CHECK(ComputeProUpServ(ValidDescriptor()).IsTriviallyValid(/*is_basic_scheme_active=*/true, state));
    }

    auto d = ValidDescriptor();
    d.nVersion = 0;
    ExpectRejected(d, "bad-compute-descriptor-version");

    d = ValidDescriptor();
    d.nVersion = CComputeServiceDescriptor::CURRENT_VERSION + 1;
    ExpectRejected(d, "bad-compute-descriptor-version");

    d = ValidDescriptor();
    d.vchOracleKey.pop_back(); // wrong length
    ExpectRejected(d, "bad-compute-oracle-key");

    d = ValidDescriptor();
    d.vchOracleKey[0] = 0x04; // uncompressed header on a 33-byte body
    ExpectRejected(d, "bad-compute-oracle-key");

    d = ValidDescriptor();
    std::fill(d.vchOracleKey.begin() + 1, d.vchOracleKey.end(), 0xff); // x above the field prime
    ExpectRejected(d, "bad-compute-oracle-key");

    d = ValidDescriptor();
    d.vchBlsReserved.assign(1, 0x01); // the reserved slot must stay empty at v1
    ExpectRejected(d, "bad-compute-bls-reserved");

    d = ValidDescriptor();
    d.certExpiryHeight = 0; // hash without expiry
    ExpectRejected(d, "bad-compute-cert");

    d = ValidDescriptor();
    d.certHash = uint256(); // expiry without hash
    ExpectRejected(d, "bad-compute-cert");
}

// A masternode state written before the descriptor existed must read back
// unchanged through the frozen no-compute format, with the descriptor at its
// default. This is the byte-level contract the evodb migration relies on.
BOOST_AUTO_TEST_CASE(old_format_state_reads_back)
{
    CDeterministicMNState_no_compute_format old_state;
    old_state.nRegisteredHeight = 123;
    old_state.nLastPaidHeight = 456;
    CKeyID owner;
    std::memset(owner.begin(), 0x22, owner.size());
    old_state.keyIDOwner = owner;

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << uint256::ONE;            // proTxHash
    uint64_t internal_id{7};
    ss << VARINT(internal_id);
    ss << COutPoint(uint256::TWO, 1);
    ss << uint16_t{0};             // nOperatorReward
    ss << old_state;
    ss << MnType::Regular;

    CDeterministicMN dmn(deserialize, ss, CDeterministicMN::MN_VERSION_FORMAT);
    BOOST_CHECK(dmn.proTxHash == uint256::ONE);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nRegisteredHeight, 123);
    BOOST_CHECK_EQUAL(dmn.pdmnState->nLastPaidHeight, 456);
    BOOST_CHECK(dmn.pdmnState->keyIDOwner == owner);
    BOOST_CHECK(dmn.pdmnState->computeDescriptor == CComputeServiceDescriptor{});
}

// The type grants five payout slots, but only a live certificate keeps them:
// the premium pays for the oracle service, not for the type itself.
BOOST_AUTO_TEST_CASE(payment_weight_is_certificate_gated)
{
    CDeterministicMNList list(uint256(), /*height=*/1000, /*totalRegisteredCount=*/1);
    auto dmn = std::make_shared<CDeterministicMN>(/*internalId=*/1, MnType::Compute);
    dmn->proTxHash = uint256::ONE;
    dmn->collateralOutpoint = COutPoint(uint256::TWO, 0);
    auto state = std::make_shared<CDeterministicMNState>();
    CKeyID owner;
    std::memset(owner.begin(), 0x11, owner.size());
    state->keyIDOwner = owner;
    state->computeDescriptor = ValidDescriptor(); // expires at 5000, list is at 1000
    dmn->pdmnState = state;
    list.AddMN(dmn);

    const auto type_weight = GetMnType(MnType::Compute).payment_weight;
    BOOST_CHECK_EQUAL(list.GetEffectivePaymentWeight(*list.GetMN(uint256::ONE)), type_weight);
    BOOST_CHECK_EQUAL(list.GetValidPaymentWeightedMNsCount(), size_t(type_weight));

    // a lapsed certificate costs the premium but not membership
    auto lapsed = std::make_shared<CDeterministicMNState>(*state);
    lapsed->computeDescriptor.certExpiryHeight = 900;
    lapsed->computeDescriptor.certHash = uint256::ONE;
    list.UpdateMN(uint256::ONE, lapsed);
    BOOST_CHECK_EQUAL(list.GetEffectivePaymentWeight(*list.GetMN(uint256::ONE)), 1);
    BOOST_CHECK_EQUAL(list.GetValidPaymentWeightedMNsCount(), size_t(1));
}

BOOST_AUTO_TEST_SUITE_END()
