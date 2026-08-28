// Copyright (c) 2021-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/trivially_invalid.json.h>
#include <test/data/trivially_valid.json.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <netbase.h>
#include <primitives/transaction.h>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

extern UniValue read_json(const std::string& jsondata);

BOOST_FIXTURE_TEST_SUITE(evo_trivialvalidation, BasicTestingSetup)

template<class T>
void TestTxHelper(const CMutableTransaction& tx, bool is_basic_bls, bool expected_failure, const std::string& expected_error)
{
    const bool payload_to_fail = expected_failure && expected_error == "gettxpayload-fail";
    const auto opt_payload = GetTxPayload<T>(tx, false);
    BOOST_CHECK_EQUAL(opt_payload.has_value(), !payload_to_fail);

    // No need to check anything else if GetTxPayload() expected to fail
    if (payload_to_fail) return;

    TxValidationState dummy_state;
    BOOST_CHECK_EQUAL(opt_payload->IsTriviallyValid(is_basic_bls, dummy_state), !expected_failure);
    if (expected_failure) {
        BOOST_CHECK_EQUAL(dummy_state.GetRejectReason(), expected_error);
    }
}

void trivialvalidation_runner(const std::string& json)
{
    const UniValue vectors = read_json(json);

    for (size_t idx = 1; idx < vectors.size(); idx++) {
        UniValue test = vectors[idx];
        uint256 txHash;
        std::string txType;
        CMutableTransaction tx;
        std::string expected_err;
        try {
            // Additional data
            txHash = uint256S(test[0].get_str());
            BOOST_TEST_MESSAGE("tx: " << test[0].get_str());
            txType = test[1].get_str();
            BOOST_CHECK(test[2].get_str() == "basic" || test[2].get_str() == "legacy");
            bool is_basic_bls = test[2].get_str() == "basic";
            // Raw transaction
            CDataStream stream(ParseHex(test[3].get_str()), SER_NETWORK, PROTOCOL_VERSION);
            stream >> tx;
            bool expected = test.size() > 4;
            if (expected) {
                expected_err = test[4].get_str();
            }
            // Sanity check
            BOOST_CHECK_EQUAL(tx.nVersion, 3);
            BOOST_CHECK_EQUAL(tx.GetHash(), txHash);
            // Deserialization based on transaction nType
            TxValidationState dummy_state;
            switch (tx.nType) {
            case TRANSACTION_PROVIDER_REGISTER: {
                BOOST_CHECK_EQUAL(txType, "proregtx");

                TestTxHelper<CProRegTx>(tx, is_basic_bls, expected, expected_err);
                break;
            }
            case TRANSACTION_PROVIDER_UPDATE_SERVICE: {
                BOOST_CHECK_EQUAL(txType, "proupservtx");

                TestTxHelper<CProUpServTx>(tx, is_basic_bls, expected, expected_err);
                break;
            }
            case TRANSACTION_PROVIDER_UPDATE_REGISTRAR: {
                BOOST_CHECK_EQUAL(txType, "proupregtx");

                TestTxHelper<CProUpRegTx>(tx, is_basic_bls, expected, expected_err);
                break;
            }
            case TRANSACTION_PROVIDER_UPDATE_REVOKE: {
                BOOST_CHECK_EQUAL(txType, "prouprevtx");

                TestTxHelper<CProUpRevTx>(tx, is_basic_bls, expected, expected_err);
                break;
            }
            default:
                // TRANSACTION_COINBASE and TRANSACTION_NORMAL
                // are not subject to trivial validation checks
                BOOST_CHECK(false);
            }
        } catch (...) {
            BOOST_ERROR("Bad test, couldn't deserialize data: " << test.write());
            continue;
        }
    }

}

BOOST_AUTO_TEST_CASE(trivialvalidation_valid)
{
    const std::string json(json_tests::trivially_valid, json_tests::trivially_valid + sizeof(json_tests::trivially_valid));

    bls::bls_legacy_scheme.store(true);
    trivialvalidation_runner(json);

    bls::bls_legacy_scheme.store(false);
    trivialvalidation_runner(json);
}

BOOST_AUTO_TEST_CASE(trivialvalidation_invalid)
{
    const std::string json(json_tests::trivially_invalid, json_tests::trivially_invalid + sizeof(json_tests::trivially_invalid));

    bls::bls_legacy_scheme.store(true);
    trivialvalidation_runner(json);

    bls::bls_legacy_scheme.store(false);
    trivialvalidation_runner(json);
}

// Regression for dash#7488: CProUpServTx::IsTriviallyValid must reject an
// out-of-range nType, matching CProRegTx and the block-connect check in
// BuildNewListFromBlock. Without this, a ProUpServTx with nType >= MnType::COUNT
// is mempool-valid but block-invalid, which stalls CreateNewBlock /
// getblocktemplate once the tx is relayed.
BOOST_AUTO_TEST_CASE(proupserv_rejects_invalid_ntype)
{
    auto make_proupserv = [](MnType nType) {
        CProUpServTx proTx;
        proTx.nVersion = CProUpServTx::BASIC_BLS_VERSION;
        proTx.nType = nType;
        proTx.addr = LookupNumeric("1.1.1.1", 1000);
        return proTx;
    };

    // Valid types must still pass trivial validation.
    {
        TxValidationState state;
        BOOST_CHECK(make_proupserv(MnType::Regular).IsTriviallyValid(/*is_basic_scheme_active=*/true, state));
        state = {};
        BOOST_CHECK(make_proupserv(MnType::Evo).IsTriviallyValid(/*is_basic_scheme_active=*/true, state));
    }

    // Out-of-range nType (raw uint16 deserialised into the enum) must be rejected.
    for (const MnType bad_type : {static_cast<MnType>(2), static_cast<MnType>(42), static_cast<MnType>(0xFFFF)}) {
        TxValidationState state;
        BOOST_CHECK(!make_proupserv(bad_type).IsTriviallyValid(/*is_basic_scheme_active=*/true, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-type");
    }
}

BOOST_AUTO_TEST_SUITE_END()
