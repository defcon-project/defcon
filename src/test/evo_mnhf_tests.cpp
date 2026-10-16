// Copyright (c) 2021-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <evo/chainhelper.h>
#include <evo/mnhftx.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <llmq/context.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>
#include <versionbits.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>


static CMutableTransaction CreateMNHFTx(const uint256& quorumHash, const CBLSSignature& cblSig, const uint8_t versionBit,
                                        const uint8_t payloadVersion = static_cast<uint8_t>(MNHFTxPayload::CURRENT_VERSION))
{
    MNHFTxPayload extraPayload;
    extraPayload.nVersion = payloadVersion;
    extraPayload.signal.versionBit = versionBit;
    extraPayload.signal.quorumHash = quorumHash;
    extraPayload.signal.sig = cblSig;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_MNHF_SIGNAL;
    SetTxPayload(tx, extraPayload);

    return tx;
}

BOOST_FIXTURE_TEST_SUITE(evo_mnhf_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(verify_mnhf_specialtx_tests)
{
    int count = 10;
    uint16_t bit = 1;

    std::vector<CBLSSignature> vec_sigs;
    std::vector<CBLSPublicKey> vec_pks;
    std::vector<CBLSSecretKey> vec_sks;

    CBLSSecretKey sk;
    for (int i = 0; i < count; i++) {
        sk.MakeNewKey();
        vec_pks.push_back(sk.GetPublicKey());
        vec_sks.push_back(sk);
    }

    CBLSSecretKey ag_sk = CBLSSecretKey::AggregateInsecure(vec_sks);
    CBLSPublicKey ag_pk = CBLSPublicKey::AggregateInsecure(vec_pks);

    BOOST_CHECK(ag_sk.IsValid());
    BOOST_CHECK(ag_pk.IsValid());

    const bool use_legacy{false};
    uint256 verHash = uint256S(ToString(bit));
    auto sig = ag_sk.Sign(verHash, use_legacy);
    BOOST_CHECK(sig.VerifyInsecure(ag_pk, verHash, use_legacy));

    auto& chainman = Assert(m_node.chainman);
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    const CBlockIndex* pindex = chainman->ActiveChain().Tip();
    uint256 hash = GetRandHash();

    { // wrong quorum (we don't have any indeed)
        const CTransaction tx{CTransaction(CreateMNHFTx(hash, sig, bit))};
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, CTransaction(tx), pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-quorum-hash");
    }

    { // non EHF fork
        const CTransaction tx{CTransaction(CreateMNHFTx(hash, sig, 28))};
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, CTransaction(tx), pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-non-ehf");
    }
}

BOOST_AUTO_TEST_CASE(check_mnhf_rejects_out_of_range_bits_before_activation)
{
    auto& chainman = Assert(m_node.chainman);
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    const CBlockIndex* pindex = chainman->ActiveChain().Tip();
    const uint256 hash = GetRandHash();

    for (const uint8_t bit : std::array<uint8_t, 2>{
             static_cast<uint8_t>(VERSIONBITS_NUM_BITS),
             std::numeric_limits<uint8_t>::max()}) {
        const CTransaction tx{CreateMNHFTx(hash, CBLSSignature{}, bit)};
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, tx, pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-nbit-out-of-bounds");
    }
}

BOOST_AUTO_TEST_CASE(check_mnhf_preserves_payload_version_and_type_rejects)
{
    auto& chainman = Assert(m_node.chainman);
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    const CBlockIndex* pindex = chainman->ActiveChain().Tip();
    const uint256 hash = GetRandHash();
    const uint8_t out_of_bounds_bit{static_cast<uint8_t>(VERSIONBITS_NUM_BITS)};

    for (const uint8_t payload_version : std::array<uint8_t, 2>{0, 2}) {
        const CTransaction tx{CreateMNHFTx(hash, CBLSSignature{}, out_of_bounds_bit, payload_version)};
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, tx, pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-version");
    }

    {
        CMutableTransaction tx{CreateMNHFTx(hash, CBLSSignature{}, 1)};
        tx.vExtraPayload.clear();
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, CTransaction(tx), pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-payload");
    }

    {
        CMutableTransaction tx{CreateMNHFTx(hash, CBLSSignature{}, 1)};
        BOOST_REQUIRE(!tx.vExtraPayload.empty());
        tx.vExtraPayload.pop_back();
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, CTransaction(tx), pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-payload");
    }

    {
        CMutableTransaction tx{CreateMNHFTx(hash, CBLSSignature{}, 1)};
        tx.vExtraPayload.push_back(0);
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, CTransaction(tx), pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-payload");
    }

    {
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_NORMAL;
        TxValidationState state;
        BOOST_CHECK(!CheckMNHFTx(*chainman, qman, CTransaction(tx), pindex, state));
        BOOST_CHECK_EQUAL(state.ToString(), "bad-mnhf-type");
    }
}

BOOST_AUTO_TEST_CASE(dispatcher_keeps_dip3_gate_before_mnhf)
{
    auto& chainman = Assert(m_node.chainman);
    auto& special_tx = *Assert(m_node.chain_helper)->special_tx;
    const CBlockIndex* pindex = chainman->ActiveChain().Tip();

    BOOST_REQUIRE(!DeploymentActiveAfter(pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0003));

    for (const uint8_t bit : std::array<uint8_t, 2>{
             static_cast<uint8_t>(VERSIONBITS_NUM_BITS),
             std::numeric_limits<uint8_t>::max()}) {
        const CTransaction tx{CreateMNHFTx(GetRandHash(), CBLSSignature{}, bit)};
        TxValidationState state;
        {
            LOCK(cs_main);
            BOOST_CHECK(!special_tx.CheckSpecialTx(tx, pindex, chainman->ActiveChainstate().CoinsTip(), true, state));
        }
        BOOST_CHECK_EQUAL(state.ToString(), "bad-tx-type-dip3-inactive");
    }
}

BOOST_AUTO_TEST_SUITE_END()

struct MNHFDip3ActiveV20InactiveSetup : public TestChainSetup {
    MNHFDip3ActiveV20InactiveSetup() :
        TestChainSetup(430)
    {
        // The next block is height 432, where regtest's DIP3 gate becomes active.
        CreateAndProcessBlock({}, coinbaseKey);
    }
};

BOOST_FIXTURE_TEST_SUITE(evo_mnhf_dispatcher_tests, MNHFDip3ActiveV20InactiveSetup)

BOOST_AUTO_TEST_CASE(dispatcher_rejects_out_of_range_bits_after_dip3_before_v20)
{
    auto& chainman = Assert(m_node.chainman);
    auto& special_tx = *Assert(m_node.chain_helper)->special_tx;
    const CBlockIndex* pindex = chainman->ActiveChain().Tip();
    const auto& consensus = Params().GetConsensus();

    BOOST_REQUIRE(DeploymentActiveAfter(pindex, consensus, Consensus::DEPLOYMENT_DIP0003));
    BOOST_REQUIRE(!DeploymentActiveAfter(pindex, consensus, Consensus::DEPLOYMENT_V20));

    const std::array<std::pair<uint8_t, const char*>, 4> cases{{
        {1, "bad-mnhf-quorum-hash"},
        {28, "bad-mnhf-non-ehf"},
        {static_cast<uint8_t>(VERSIONBITS_NUM_BITS), "bad-mnhf-nbit-out-of-bounds"},
        {std::numeric_limits<uint8_t>::max(), "bad-mnhf-nbit-out-of-bounds"},
    }};

    for (const auto& [bit, reason] : cases) {
        const CTransaction tx{CreateMNHFTx(GetRandHash(), CBLSSignature{}, bit)};
        TxValidationState state;
        {
            LOCK(cs_main);
            BOOST_CHECK(!special_tx.CheckSpecialTx(tx, pindex, chainman->ActiveChainstate().CoinsTip(), true, state));
        }
        BOOST_CHECK_EQUAL(state.ToString(), reason);
    }
}

BOOST_AUTO_TEST_SUITE_END()
