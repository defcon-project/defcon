// Copyright (c) 2021-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chainparams.h>

#include <limits>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <messagesigner.h>
#include <netbase.h>
#include <node/miner.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <validation.h>

#include <evo/deterministicmns.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <masternode/payments.h>
#include <util/enumerate.h>
#include <util/irange.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

using SimpleUTXOMap = std::map<COutPoint, std::pair<int, CAmount>>;

struct TestChainBRRBeforeActivationSetup : public TestChainSetup
{
    // Force fast DIP3 activation
    TestChainBRRBeforeActivationSetup() :
        TestChainSetup(497, {"-dip3params=30:50", "-testactivationheight=brr@1000", "-testactivationheight=v20@1200",
                             "-testactivationheight=mn_rr@2200"})
    {
    }
};

static SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransactionRef>& txs)
{
    SimpleUTXOMap utxos;
    for (auto [i, tx] : enumerate(txs)) {
        for (auto [j, output] : enumerate(tx->vout)) {
            utxos.try_emplace(COutPoint(tx->GetHash(), j), std::make_pair((int)i + 1, output.nValue));
        }
    }
    return utxos;
}

static std::vector<COutPoint> SelectUTXOs(const CChain& active_chain, SimpleUTXOMap& utoxs, CAmount amount, CAmount& changeRet)
{
    changeRet = 0;

    std::vector<COutPoint> selectedUtxos;
    CAmount selectedAmount = 0;
    while (!utoxs.empty()) {
        bool found = false;
        for (auto it = utoxs.begin(); it != utoxs.end(); ++it) {
            if (active_chain.Height() - it->second.first < 101) {
                continue;
            }

            found = true;
            selectedAmount += it->second.second;
            selectedUtxos.emplace_back(it->first);
            utoxs.erase(it);
            break;
        }
        BOOST_REQUIRE(found);
        if (selectedAmount >= amount) {
            changeRet = selectedAmount - amount;
            break;
        }
    }

    return selectedUtxos;
}

static void FundTransaction(const CChain& active_chain, CMutableTransaction& tx, SimpleUTXOMap& utoxs, const CScript& scriptPayout, CAmount amount)
{
    CAmount change;
    auto inputs = SelectUTXOs(active_chain, utoxs, amount, change);
    for (const auto& input : inputs) {
        tx.vin.emplace_back(input);
    }
    tx.vout.emplace_back(amount, scriptPayout);
    if (change != 0) {
        tx.vout.emplace_back(change, scriptPayout);
    }
}

static void SignTransaction(const CTxMemPool& mempool, CMutableTransaction& tx, const CKey& coinbaseKey)
{
    FillableSigningProvider tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

    for (auto [i, input] : enumerate(tx.vin)) {
        uint256 hashBlock;
        CTransactionRef txFrom = GetTransaction(/* block_index */ nullptr, &mempool, input.prevout.hash, Params().GetConsensus(), hashBlock);
        BOOST_REQUIRE(txFrom);
        BOOST_REQUIRE(SignSignature(tempKeystore, *txFrom, tx, i, SIGHASH_ALL));
    }
}

static CMutableTransaction CreateProRegTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, int port, const CScript& scriptPayout, const CKey& coinbaseKey, CKey& ownerKeyRet, CBLSSecretKey& operatorKeyRet)
{
    ownerKeyRet.MakeNewKey(true);
    operatorKeyRet.MakeNewKey();

    CProRegTx proTx;
    proTx.nVersion = CProRegTx::GetVersion(!bls::bls_legacy_scheme);
    proTx.collateralOutpoint.n = 0;
    proTx.addr = LookupNumeric("1.1.1.1", port);
    proTx.keyIDOwner = ownerKeyRet.GetPubKey().GetID();
    proTx.pubKeyOperator.Set(operatorKeyRet.GetPublicKey(), bls::bls_legacy_scheme.load());
    proTx.keyIDVoting = ownerKeyRet.GetPubKey().GetID();
    proTx.scriptPayout = scriptPayout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    FundTransaction(active_chain, tx, utxos, scriptPayout, dmn_types::BuildMnStruct(MnType::Regular).collat_amount);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

BOOST_AUTO_TEST_SUITE(block_reward_reallocation_tests)

BOOST_FIXTURE_TEST_CASE(block_reward_reallocation, TestChainBRRBeforeActivationSetup)
{
    auto& dmnman = *Assert(m_node.dmnman);
    const auto& consensus_params = Params().GetConsensus();

    CScript coinbasePubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    BOOST_REQUIRE(DeploymentDIP0003Enforced(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()),
                                            consensus_params));

    // Register one MN
    CKey ownerKey;
    CBLSSecretKey operatorKey;
    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);
    auto tx = CreateProRegTx(m_node.chainman->ActiveChain(), *m_node.mempool, utxos, 1, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey);

    CreateAndProcessBlock({tx}, coinbaseKey);

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        dmnman.UpdatedBlockTip(tip);

        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));

        BOOST_CHECK_EQUAL(tip->nHeight, 498);
        BOOST_CHECK(tip->nHeight < Params().GetConsensus().BRRHeight);
    }

    CreateAndProcessBlock({}, coinbaseKey);

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_CHECK_EQUAL(tip->nHeight, 499);
        dmnman.UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
        BOOST_CHECK(tip->nHeight < Params().GetConsensus().BRRHeight);
        // Creating blocks by different ways
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, *m_node.mempool, Params()).CreateNewBlock(coinbasePubKey);
    }
    for ([[maybe_unused]] auto _ : irange::range(499)) {
        CreateAndProcessBlock({}, coinbaseKey);
        LOCK(cs_main);
        dmnman.UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    BOOST_CHECK(m_node.chainman->ActiveChain().Height() < Params().GetConsensus().BRRHeight);
    CreateAndProcessBlock({}, coinbaseKey);

    {
        // Advance to ACTIVE at height = (BRRHeight - 1)
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_CHECK_EQUAL(tip->nHeight, Params().GetConsensus().BRRHeight - 1);
        dmnman.UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
    }

    {
        // Reward split should stay ~50/50 before the first superblock after activation.
        // This applies even if reallocation was activated right at superblock height like it does here.
        // next block should be signaling by default
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        const bool isV20Active{DeploymentActiveAfter(tip, consensus_params, Consensus::DEPLOYMENT_V20)};
        dmnman.UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, isV20Active);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, *m_node.mempool, Params()).CreateNewBlock(coinbasePubKey);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
    }

    for ([[maybe_unused]] auto _ : irange::range(consensus_params.nSuperblockCycle - 1)) {
        CreateAndProcessBlock({}, coinbaseKey);
    }

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        const bool isV20Active{DeploymentActiveAfter(tip, consensus_params, Consensus::DEPLOYMENT_V20)};
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, isV20Active);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, *m_node.mempool, Params()).CreateNewBlock(coinbasePubKey);
        // The amounts here were Dash's, from a subsidy that falls with
        // difficulty and a masternode share that follows it. This chain pays a
        // flat subsidy and a flat 10,000 to the masternode, so the numbers are
        // read from the rule rather than typed.
        BOOST_CHECK_EQUAL(masternode_payment, 10000 * COIN);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
        BOOST_CHECK_EQUAL(pblocktemplate->block.vtx[0]->GetValueOut(), block_subsidy);
    }

    // There is no reallocation on this chain, and that is the thing worth
    // pinning. Dash moves the masternode share from 50% to 75% over 19
    // adjustments of three superblocks each, then again to platform once
    // MN_RR activates, and the suite this replaces mined more than 1500 blocks
    // to walk that schedule -- asserting amounts from a subsidy formula this
    // fork deleted, and eventually failing to mine at all with time-too-new.
    //
    // Here GetBlockSubsidyHelper returns a flat amount and GetMasternodePayment
    // a flat 10,000, so the share cannot move. One more superblock cycle is
    // enough to say so.
    for ([[maybe_unused]] auto _ : irange::range(consensus_params.nSuperblockCycle)) {
        CreateAndProcessBlock({}, coinbaseKey);
        LOCK(cs_main);
        dmnman.UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_CHECK(tip->nHeight > consensus_params.BRRHeight);
        const bool isV20Active{DeploymentActiveAfter(tip, consensus_params, Consensus::DEPLOYMENT_V20)};
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount block_subsidy_sb = GetSuperblockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, isV20Active);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, *m_node.mempool, Params()).CreateNewBlock(coinbasePubKey);

        // Past the height that would start the reallocation, and nothing moved.
        BOOST_CHECK_EQUAL(masternode_payment, 10000 * COIN);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
        BOOST_CHECK_EQUAL(pblocktemplate->block.vtx[0]->GetValueOut(), block_subsidy);
        // Nothing is carved out for a treasury either, on either side of v20.
        BOOST_CHECK_EQUAL(block_subsidy_sb, 0);
        BOOST_CHECK_EQUAL(GetSuperblockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, !isV20Active), 0);
        // And the deployment flag does not change what a block pays.
        BOOST_CHECK_EQUAL(GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, !isV20Active), block_subsidy);
        BOOST_CHECK_EQUAL(GetMasternodePayment(tip->nHeight, block_subsidy, !isV20Active), masternode_payment);
    }
}

/**
 * No real network can reach the reallocation height at all.
 *
 * Regtest sets BRRHeight to 1 so the case above can cross it; main, testnet and
 * devnet leave it at the largest int, which is the fork's way of saying the
 * feature is off. Pinned because a height set here by accident would change
 * what every block pays.
 */
BOOST_FIXTURE_TEST_CASE(reallocation_is_unreachable_on_every_real_network, BasicTestingSetup)
{
    for (const std::string& net : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        const auto params = CreateChainParams(*m_node.args, net);
        BOOST_CHECK_EQUAL(params->GetConsensus().BRRHeight, std::numeric_limits<int>::max());
    }
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(*m_node.args, CBaseChainParams::DEVNET)->GetConsensus().BRRHeight,
                      std::numeric_limits<int>::max());
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_SUITE_END()
