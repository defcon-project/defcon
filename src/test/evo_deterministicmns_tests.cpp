// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <base58.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <messagesigner.h>
#include <net.h>
#include <net_processing.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/chainstate.h>
#include <policy/policy.h>
#include <protocol.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <shutdown.h>
#include <spork.h>
#include <txmempool.h>
#include <validation.h>
#include <warnings.h>

#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/simplifiedmns.h>
#include <llmq/context.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <llmq/context.h>
#include <llmq/dkgsession.h>
#include <llmq/instantsend.h>
#include <llmq/utils.h>

#include <boost/test/unit_test.hpp>

using SimpleUTXOMap = std::map<COutPoint, std::pair<int, CAmount>>;

static SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransactionRef>& txs)
{
    SimpleUTXOMap utxos;
    for (size_t i = 0; i < txs.size(); i++) {
        auto& tx = txs[i];
        for (size_t j = 0; j < tx->vout.size(); j++) {
            utxos.emplace(COutPoint(tx->GetHash(), j), std::make_pair((int)i + 1, tx->vout[j].nValue));
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

static void FundTransaction(const CChain& active_chain, CMutableTransaction& tx, SimpleUTXOMap& utoxs, const CScript& scriptPayout, CAmount amount, const CKey& coinbaseKey)
{
    CAmount change;
    auto inputs = SelectUTXOs(active_chain, utoxs, amount, change);
    for (size_t i = 0; i < inputs.size(); i++) {
        tx.vin.emplace_back(CTxIn(inputs[i]));
    }
    tx.vout.emplace_back(CTxOut(amount, scriptPayout));
    if (change != 0) {
        tx.vout.emplace_back(CTxOut(change, scriptPayout));
    }
}

static void SignTransaction(const CTxMemPool& mempool, CMutableTransaction& tx, const CKey& coinbaseKey)
{
    FillableSigningProvider tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        uint256 hashBlock;
        CTransactionRef txFrom = GetTransaction(/* block_index */ nullptr, &mempool, tx.vin[i].prevout.hash, Params().GetConsensus(), hashBlock);
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
    FundTransaction(active_chain, tx, utxos, scriptPayout, dmn_types::BuildMnStruct(MnType::Regular).collat_amount, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpServTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, int port, const CScript& scriptOperatorPayout, const CKey& coinbaseKey)
{
    CProUpServTx proTx;
    proTx.nVersion = CProUpRevTx::GetVersion(!bls::bls_legacy_scheme);
    proTx.proTxHash = proTxHash;
    proTx.addr = LookupNumeric("1.1.1.1", port);
    proTx.scriptOperatorPayout = scriptOperatorPayout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_SERVICE;
    FundTransaction(active_chain, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    proTx.sig = operatorKey.Sign(::SerializeHash(proTx), bls::bls_legacy_scheme);
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRegTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, const uint256& proTxHash, const CKey& mnKey, const CBLSPublicKey& pubKeyOperator, const CKeyID& keyIDVoting, const CScript& scriptPayout, const CKey& coinbaseKey)
{
    CProUpRegTx proTx;
    proTx.nVersion = CProUpRegTx::GetVersion(!bls::bls_legacy_scheme);
    proTx.proTxHash = proTxHash;
    proTx.pubKeyOperator.Set(pubKeyOperator, bls::bls_legacy_scheme.load());
    proTx.keyIDVoting = keyIDVoting;
    proTx.scriptPayout = scriptPayout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;
    FundTransaction(active_chain, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    CHashSigner::SignHash(::SerializeHash(proTx), mnKey, proTx.vchSig);
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRevTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, const CKey& coinbaseKey)
{
    CProUpRevTx proTx;
    proTx.nVersion = CProUpRevTx::GetVersion(!bls::bls_legacy_scheme);
    proTx.proTxHash = proTxHash;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_REVOKE;
    FundTransaction(active_chain, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    proTx.sig = operatorKey.Sign(::SerializeHash(proTx), bls::bls_legacy_scheme);
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

template<typename ProTx>
static CMutableTransaction MalleateProTxPayout(const CMutableTransaction& tx)
{
    auto opt_protx = GetTxPayload<ProTx>(tx);
    BOOST_REQUIRE(opt_protx.has_value());
    auto& protx = *opt_protx;

    CKey key;
    key.MakeNewKey(false);
    protx.scriptPayout = GetScriptForDestination(PKHash(key.GetPubKey()));

    CMutableTransaction tx2 = tx;
    SetTxPayload(tx2, protx);

    return tx2;
}

// A masternode collateral cannot be spent for minStaticCollateral blocks after
// registration (8,064 -- about two weeks). Fixtures that exist to exercise what
// happens *after* a collateral is spent would have to mine that many blocks to
// reach it, so they suspend the maturity for the one block that does the spend
// and restore it immediately. The rule itself is covered by collateral_tests.
struct ScopedCollateralMaturityOverride {
    explicit ScopedCollateralMaturityOverride(Consensus::Params& params) :
        m_params(params), m_saved(params.minStaticCollateral)
    {
        m_params.minStaticCollateral = 0;
    }
    ~ScopedCollateralMaturityOverride() { m_params.minStaticCollateral = m_saved; }
    Consensus::Params& m_params;
    const int m_saved;
};

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

static CDeterministicMNCPtr FindPayoutDmn(CDeterministicMNManager& dmnman, const CBlock& block)
{
    auto dmnList = dmnman.GetListAtChainTip();

    for (const auto& txout : block.vtx[0]->vout) {
        CDeterministicMNCPtr found;
        dmnList.ForEachMNShared(true, [&](const CDeterministicMNCPtr& dmn) {
            if (found == nullptr && txout.scriptPubKey == dmn->pdmnState->scriptPayout) {
                found = dmn;
            }
        });
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

static bool CheckTransactionSignature(const CTxMemPool& mempool, const CMutableTransaction& tx)
{
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const auto& txin = tx.vin[i];
        uint256 hashBlock;
        CTransactionRef txFrom = GetTransaction(/* block_index */ nullptr, &mempool, txin.prevout.hash, Params().GetConsensus(), hashBlock);
        BOOST_REQUIRE(txFrom);

        CAmount amount = txFrom->vout[txin.prevout.n].nValue;
        if (!VerifyScript(txin.scriptSig, txFrom->vout[txin.prevout.n].scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&tx, i, amount, MissingDataBehavior::ASSERT_FAIL))) {
            return false;
        }
    }
    return true;
}

void FuncDIP3Activation(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey ownerKey;
    CBLSSecretKey operatorKey;
    CTxDestination payoutDest = DecodeDestination("yRq1Ky1AfFmf597rnotj7QRxsDUKePVWNF");
    auto tx = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, GetScriptForDestination(payoutDest), setup.coinbaseKey, ownerKey, operatorKey);
    std::vector<CMutableTransaction> txns = {tx};

    int nHeight = chainman.ActiveChain().Height();

    // We start one block before DIP3 activation, so mining a block with a DIP3 transaction should fail
    auto block = std::make_shared<CBlock>(setup.CreateBlock(txns, setup.coinbaseKey, chainman.ActiveChainstate()));
    chainman.ProcessNewBlock(Params(), block, true, nullptr);
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight);
    BOOST_REQUIRE(block->GetHash() != chainman.ActiveChain().Tip()->GetBlockHash());
    BOOST_REQUIRE(!dmnman.GetListAtChainTip().HasMN(tx.GetHash()));

    // This block should activate DIP3
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    // Mining a block with a DIP3 transaction should succeed now
    block = std::make_shared<CBlock>(setup.CreateBlock(txns, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 2);
    BOOST_CHECK_EQUAL(block->GetHash(), chainman.ActiveChain().Tip()->GetBlockHash());
    BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
};

void FuncV19Activation(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);

    BOOST_REQUIRE(!DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));

    // create
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    CKey collateral_key;
    collateral_key.MakeNewKey(true);
    auto collateralScript = GetScriptForDestination(PKHash(collateral_key.GetPubKey()));
    auto tx_reg = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, collateralScript, setup.coinbaseKey, owner_key, operator_key);
    auto tx_reg_hash = tx_reg.GetHash();

    int nHeight = chainman.ActiveChain().Height();

    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_reg}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    BOOST_REQUIRE(!DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    dmnman.DoMaintenance();
    auto tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(tip_list.HasMN(tx_reg_hash));
    auto pindex_create = chainman.ActiveChain().Tip();
    auto base_list = dmnman.GetListForBlock(pindex_create);
    std::vector<CDeterministicMNListDiff> diffs;

    // update
    CBLSSecretKey operator_key_new;
    operator_key_new.MakeNewKey();
    auto tx_upreg = CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, tx_reg_hash, owner_key, operator_key_new.GetPublicKey(), owner_key.GetPubKey().GetID(), collateralScript, setup.coinbaseKey);

    block = std::make_shared<CBlock>(setup.CreateBlock({tx_upreg}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    BOOST_REQUIRE(!DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    dmnman.DoMaintenance();
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(tip_list.HasMN(tx_reg_hash));
    diffs.push_back(base_list.BuildDiff(tip_list));

    // spend
    CMutableTransaction tx_spend;
    COutPoint collateralOutpoint(tx_reg_hash, 0);
    tx_spend.vin.emplace_back(collateralOutpoint);
    tx_spend.vout.emplace_back(999.99 * COIN, collateralScript);

    FillableSigningProvider signing_provider;
    signing_provider.AddKeyPubKey(collateral_key, collateral_key.GetPubKey());
    // Use the full coin context for signing. The legacy single-transaction
    // overload does not prepare the spent-output data required by the modern
    // signature path.
    std::map<COutPoint, Coin> collateral_coins;
    collateral_coins.emplace(collateralOutpoint, Coin(tx_reg.vout[0], nHeight, /* fCoinBaseIn */ false, /* fCoinStakeIn */ false));
    std::map<int, bilingual_str> input_errors;
    BOOST_REQUIRE(::SignTransaction(tx_spend, &signing_provider, collateral_coins, SIGHASH_ALL, input_errors));
    const int expected_min_static_collateral = Params().GetConsensus().minStaticCollateral;
    {
        // This fixture verifies the pre-V19 deterministic list diff generated
        // by spending collateral. The current 8,064-block collateral maturity
        // rule is covered separately and would otherwise make this historical
        // fixture intractably slow.
        ScopedCollateralMaturityOverride collateral_maturity_override{
            const_cast<Consensus::Params&>(Params().GetConsensus())};

        block = std::make_shared<CBlock>(setup.CreateBlock({tx_spend}, setup.coinbaseKey, chainman.ActiveChainstate()));
        BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    }
    BOOST_CHECK_EQUAL(Params().GetConsensus().minStaticCollateral, expected_min_static_collateral);
    BOOST_REQUIRE(!DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    dmnman.DoMaintenance();
    diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
    BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));

    // mine another block so that it's not the last one before V19
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    BOOST_REQUIRE(!DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    dmnman.DoMaintenance();
    diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
    BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));

    // this block should activate V19
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    BOOST_REQUIRE(DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    dmnman.DoMaintenance();
    diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
    BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));

    // check mn list/diff
    CDeterministicMNListDiff dummy_diff = base_list.BuildDiff(tip_list);
    CDeterministicMNList dummmy_list = base_list.ApplyDiff(chainman.ActiveChain().Tip(), dummy_diff);
    // Lists should match
    BOOST_REQUIRE(dummmy_list == tip_list);

    // mine 10 more blocks
    for (int i = 0; i < 10; ++i)
    {
        setup.CreateAndProcessBlock({}, setup.coinbaseKey);
        BOOST_REQUIRE(
            DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_V19));
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1 + i);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
        dmnman.DoMaintenance();
        diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
        tip_list = dmnman.GetListAtChainTip();
        BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
        BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));
    }

    // check mn list/diff
    const CBlockIndex* v19_index = chainman.ActiveChain().Tip()->GetAncestor(Params().GetConsensus().V19Height);
    auto v19_list = dmnman.GetListForBlock(v19_index);
    dummy_diff = v19_list.BuildDiff(tip_list);
    dummmy_list = v19_list.ApplyDiff(chainman.ActiveChain().Tip(), dummy_diff);
    BOOST_REQUIRE(dummmy_list == tip_list);

    // NOTE: this fails on v19/v19.1 with errors like:
    // "RemoveMN: Can't delete a masternode ... with a pubKeyOperator=..."
    dummy_diff = base_list.BuildDiff(tip_list);
    dummmy_list = base_list.ApplyDiff(chainman.ActiveChain().Tip(), dummy_diff);
    BOOST_REQUIRE(dummmy_list == tip_list);

    dummmy_list = base_list;
    for (const auto& diff : diffs) {
        dummmy_list = dummmy_list.ApplyDiff(chainman.ActiveChain().Tip(), diff);
    }
    BOOST_REQUIRE(dummmy_list == tip_list);
};

void FuncDIP3Protx(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);

    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    setup.m_node.sporkman->SetSporkAddress(EncodeDestination(PKHash(sporkKey.GetPubKey())));
    setup.m_node.sporkman->SetPrivKey(EncodeSecret(sporkKey));

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    int nHeight = chainman.ActiveChain().Height();
    int port = 1;

    std::vector<uint256> dmnHashes;
    std::map<uint256, CKey> ownerKeys;
    std::map<uint256, CBLSSecretKey> operatorKeys;

    // register one MN per block
    for (size_t i = 0; i < 6; i++) {
        CKey ownerKey;
        CBLSSecretKey operatorKey;
        auto tx = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, port++, GenerateRandomAddress(), setup.coinbaseKey, ownerKey, operatorKey);
        dmnHashes.emplace_back(tx.GetHash());
        ownerKeys.emplace(tx.GetHash(), ownerKey);
        operatorKeys.emplace(tx.GetHash(), operatorKey);

        // also verify that payloads are not malleable after they have been signed
        // the form of ProRegTx we use here is one with a collateral included, so there is no signature inside the
        // payload itself. This means, we need to rely on script verification, which takes the hash of the extra payload
        // into account
        auto tx2 = MalleateProTxPayout<CProRegTx>(tx);
        TxValidationState dummy_state;
        // Technically, the payload is still valid...
        {
            LOCK(cs_main);
            BOOST_REQUIRE(CheckProRegTx(dmnman, CTransaction(tx), chainman.ActiveChain().Tip(), dummy_state,
                                        chainman.ActiveChainstate().CoinsTip(), true));
            BOOST_REQUIRE(CheckProRegTx(dmnman, CTransaction(tx2), chainman.ActiveChain().Tip(), dummy_state,
                                        chainman.ActiveChainstate().CoinsTip(), true));
        }
        // But the signature should not verify anymore
        BOOST_REQUIRE(CheckTransactionSignature(*(setup.m_node.mempool), tx));
        BOOST_REQUIRE(!CheckTransactionSignature(*(setup.m_node.mempool), tx2));

        setup.CreateAndProcessBlock({tx}, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());

        BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));

        nHeight++;
    }

    int DIP0003EnforcementHeightBackup = Params().GetConsensus().DIP0003EnforcementHeight;
    const_cast<Consensus::Params&>(Params().GetConsensus()).DIP0003EnforcementHeight = chainman.ActiveChain().Height() + 1;
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    nHeight++;

    // check MN reward payments
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = dmnman.GetListAtChainTip().GetMNPayee(chainman.ActiveChain().Tip());
        BOOST_ASSERT(dmnExpectedPayee);

        CBlock block = setup.CreateAndProcessBlock({}, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
        BOOST_REQUIRE(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(dmnman, block);
        BOOST_REQUIRE(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }

    // register multiple MNs per block
    for (size_t i = 0; i < 3; i++) {
        std::vector<CMutableTransaction> txns;
        for (size_t j = 0; j < 3; j++) {
            CKey ownerKey;
            CBLSSecretKey operatorKey;
            auto tx = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, port++, GenerateRandomAddress(), setup.coinbaseKey, ownerKey, operatorKey);
            dmnHashes.emplace_back(tx.GetHash());
            ownerKeys.emplace(tx.GetHash(), ownerKey);
            operatorKeys.emplace(tx.GetHash(), operatorKey);
            txns.emplace_back(tx);
        }
        setup.CreateAndProcessBlock(txns, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
        BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);

        for (size_t j = 0; j < 3; j++) {
            BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(txns[j].GetHash()));
        }

        nHeight++;
    }

    // test ProUpServTx
    auto tx = CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], operatorKeys[dmnHashes[0]], 1000, CScript(), setup.coinbaseKey);
    setup.CreateAndProcessBlock({tx}, setup.coinbaseKey);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    nHeight++;

    auto dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_REQUIRE(dmn != nullptr && dmn->pdmnState->addr.GetPort() == 1000);

    // test ProUpRevTx
    tx = CreateProUpRevTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], operatorKeys[dmnHashes[0]], setup.coinbaseKey);
    setup.CreateAndProcessBlock({tx}, setup.coinbaseKey);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    nHeight++;

    dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_REQUIRE(dmn != nullptr && dmn->pdmnState->GetBannedHeight() == nHeight);

    // test that the revoked MN does not get paid anymore
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = dmnman.GetListAtChainTip().GetMNPayee(chainman.ActiveChain().Tip());
        BOOST_REQUIRE(dmnExpectedPayee && dmnExpectedPayee->proTxHash != dmnHashes[0]);

        CBlock block = setup.CreateAndProcessBlock({}, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
        BOOST_REQUIRE(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(dmnman, block);
        BOOST_REQUIRE(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }

    // test reviving the MN
    CBLSSecretKey newOperatorKey;
    newOperatorKey.MakeNewKey();
    dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    tx = CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], ownerKeys[dmnHashes[0]], newOperatorKey.GetPublicKey(), ownerKeys[dmnHashes[0]].GetPubKey().GetID(), dmn->pdmnState->scriptPayout, setup.coinbaseKey);
    // check malleability protection again, but this time by also relying on the signature inside the ProUpRegTx
    auto tx2 = MalleateProTxPayout<CProUpRegTx>(tx);
    TxValidationState dummy_state;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(CheckProUpRegTx(dmnman, CTransaction(tx), chainman.ActiveChain().Tip(), dummy_state,
                                      chainman.ActiveChainstate().CoinsTip(), true));
        BOOST_REQUIRE(!CheckProUpRegTx(dmnman, CTransaction(tx2), chainman.ActiveChain().Tip(), dummy_state,
                                       chainman.ActiveChainstate().CoinsTip(), true));
    }
    BOOST_REQUIRE(CheckTransactionSignature(*(setup.m_node.mempool), tx));
    BOOST_REQUIRE(!CheckTransactionSignature(*(setup.m_node.mempool), tx2));
    // now process the block
    setup.CreateAndProcessBlock({tx}, setup.coinbaseKey);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    nHeight++;

    tx = CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], newOperatorKey, 100, CScript(), setup.coinbaseKey);
    setup.CreateAndProcessBlock({tx}, setup.coinbaseKey);
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    nHeight++;

    dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_REQUIRE(dmn != nullptr && dmn->pdmnState->addr.GetPort() == 100);
    BOOST_REQUIRE(dmn != nullptr && !dmn->pdmnState->IsBanned());

    // test that the revived MN gets payments again
    bool foundRevived = false;
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = dmnman.GetListAtChainTip().GetMNPayee(chainman.ActiveChain().Tip());
        BOOST_ASSERT(dmnExpectedPayee);
        if (dmnExpectedPayee->proTxHash == dmnHashes[0]) {
            foundRevived = true;
        }

        CBlock block = setup.CreateAndProcessBlock({}, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
        BOOST_REQUIRE(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(dmnman, block);
        BOOST_REQUIRE(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }
    BOOST_REQUIRE(foundRevived);

    const_cast<Consensus::Params&>(Params().GetConsensus()).DIP0003EnforcementHeight = DIP0003EnforcementHeightBackup;
}

void FuncTestMempoolReorg(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());

    int nHeight = chainman.ActiveChain().Height();
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Create a MN with an external collateral
    CMutableTransaction tx_collateral;
    FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, scriptCollateral, dmn_types::BuildMnStruct(MnType::Regular).collat_amount, setup.coinbaseKey);
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);

    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_collateral}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    setup.m_node.dmnman->UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    BOOST_CHECK_EQUAL(block->GetHash(), chainman.ActiveChain().Tip()->GetBlockHash());

    CProRegTx payload;
    payload.nVersion = CProRegTx::GetVersion(!bls::bls_legacy_scheme);
    payload.addr = LookupNumeric("1.1.1.1", 1);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::BuildMnStruct(MnType::Regular).collat_amount) {
            payload.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = 3;
    tx_reg.nType = TRANSACTION_PROVIDER_REGISTER;
    FundTransaction(chainman.ActiveChain(), tx_reg, utxos, scriptPayout, dmn_types::BuildMnStruct(MnType::Regular).collat_amount, setup.coinbaseKey);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg, payload);
    SignTransaction(*(setup.m_node.mempool), tx_reg, setup.coinbaseKey);

    CTxMemPool testPool;
    if (setup.m_node.dmnman) {
        testPool.ConnectManagers(setup.m_node.dmnman.get(), setup.m_node.llmq_ctx->isman.get());
    }
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    // Create ProUpServ and test block reorg which double-spend ProRegTx
    auto tx_up_serv = CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, tx_reg.GetHash(), operatorKey, 2, CScript(), setup.coinbaseKey);
    testPool.addUnchecked(entry.FromTx(tx_up_serv));
    // A disconnected block would insert ProRegTx back into mempool
    testPool.addUnchecked(entry.FromTx(tx_reg));
    BOOST_CHECK_EQUAL(testPool.size(), 2U);

    // Create a tx that will double-spend ProRegTx
    CMutableTransaction tx_reg_ds;
    tx_reg_ds.vin = tx_reg.vin;
    tx_reg_ds.vout.emplace_back(0, CScript() << OP_RETURN);
    SignTransaction(*(setup.m_node.mempool), tx_reg_ds, setup.coinbaseKey);

    // Check mempool as if a new block with tx_reg_ds was connected instead of the old one with tx_reg
    std::vector<CTransactionRef> block_reorg;
    block_reorg.emplace_back(std::make_shared<CTransaction>(tx_reg_ds));
    testPool.removeForBlock(block_reorg, nHeight + 2);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}

// Regression test: a ProUpRev/ProUpReg invalidates every pending ProTx of the same masternode, and
// removeProTxKeyChangedConflicts() collects those into a std::set<uint256> before removing any of
// them. When one conflicting TX is an in-mempool descendant of another, removeRecursive() on the
// ancestor also erases the descendant, so the descendant's txid in the snapshot no longer resolves.
// Dereferencing that stale iterator aborted the node; it must be skipped instead.
//
// Reachable from CTxMemPool::removeForBlock() (any block carrying such a ProTx).
void FuncTestMempoolProTxKeyChangedConflictChain(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    const CScript scriptPayout = GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey()));

    CKey ownerKey;
    CBLSSecretKey operatorKey;
    // Only the resulting proTxHash matters here; the registration never has to be mined because
    // none of the paths under test consult the masternode list for a ProUpServ payload.
    auto tx_reg = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, scriptPayout,
                                 setup.coinbaseKey, ownerKey, operatorKey);
    const uint256 proTxHash = tx_reg.GetHash();

    // Parent ProUpServ for that masternode.
    auto tx_parent = CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, proTxHash,
                                       operatorKey, 2, CScript(), setup.coinbaseKey);
    BOOST_REQUIRE(!tx_parent.vout.empty());

    // Child ProUpServ for the same masternode, spending the parent's first output.
    FillableSigningProvider keystore;
    BOOST_REQUIRE(keystore.AddKeyPubKey(setup.coinbaseKey, setup.coinbaseKey.GetPubKey()));

    CMutableTransaction tx_child;
    tx_child.nVersion = 3;
    tx_child.nType = TRANSACTION_PROVIDER_UPDATE_SERVICE;
    tx_child.vin.emplace_back(COutPoint(tx_parent.GetHash(), 0));
    tx_child.vout.emplace_back(0, scriptPayout); // value assigned by the grind loop below

    CProUpServTx payload;
    payload.nVersion = CProUpServTx::GetVersion(!bls::bls_legacy_scheme);
    payload.proTxHash = proTxHash;
    payload.addr = LookupNumeric("1.1.1.1", 3);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_child));
    payload.sig = operatorKey.Sign(::SerializeHash(payload), bls::bls_legacy_scheme);
    SetTxPayload(tx_child, payload);

    // Grind the child's output value until the parent's txid sorts before the child's: the
    // snapshot is an ordered set, so this makes the parent get removed first, taking the child
    // with it, and only then is the child's now-stale txid revisited. Only the output value has to
    // change -- inputsHash covers the inputs alone, so the payload and its BLS signature stay put,
    // and re-signing replaces the scriptSig outright.
    bool parent_sorts_first{false};
    for (CAmount fee = 1000; fee < 2000 && !parent_sorts_first; ++fee) {
        tx_child.vout[0].nValue = tx_parent.vout[0].nValue - fee;
        BOOST_REQUIRE(SignSignature(keystore, CTransaction(tx_parent), tx_child, 0, SIGHASH_ALL));
        parent_sorts_first = tx_parent.GetHash() < tx_child.GetHash();
    }
    BOOST_REQUIRE(parent_sorts_first);

    // The revocation that invalidates both pending ProUpServ transactions.
    auto tx_revoke = CreateProUpRevTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, proTxHash, operatorKey,
                                      setup.coinbaseKey);

    CTxMemPool testPool;
    BOOST_REQUIRE(setup.m_node.dmnman);
    testPool.ConnectManagers(setup.m_node.dmnman.get(), setup.m_node.llmq_ctx->isman.get());
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    testPool.addUnchecked(entry.FromTx(tx_parent));
    testPool.addUnchecked(entry.FromTx(tx_child));
    BOOST_CHECK_EQUAL(testPool.size(), 2U);

    // Pre-fix this aborted the process instead of returning.
    std::vector<CTransactionRef> block_txs{std::make_shared<CTransaction>(tx_revoke)};
    testPool.removeForBlock(block_txs, chainman.ActiveChain().Height() + 1);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}

void FuncTestMempoolDualProregtx(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    // Create a MN
    CKey ownerKey1;
    CBLSSecretKey operatorKey1;
    auto tx_reg1 = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, GenerateRandomAddress(), setup.coinbaseKey, ownerKey1, operatorKey1);

    // Create a MN with an external collateral that references tx_reg1
    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));

    CProRegTx payload;
    payload.addr = LookupNumeric("1.1.1.1", 2);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_reg1.vout.size(); ++i) {
        if (tx_reg1.vout[i].nValue == dmn_types::BuildMnStruct(MnType::Regular).collat_amount) {
            payload.collateralOutpoint = COutPoint(tx_reg1.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg2;
    tx_reg2.nVersion = 3;
    tx_reg2.nType = TRANSACTION_PROVIDER_REGISTER;
    FundTransaction(chainman.ActiveChain(), tx_reg2, utxos, scriptPayout, dmn_types::BuildMnStruct(MnType::Regular).collat_amount, setup.coinbaseKey);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg2));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg2, payload);
    SignTransaction(*(setup.m_node.mempool), tx_reg2, setup.coinbaseKey);

    CTxMemPool testPool;
    if (setup.m_node.dmnman) {
        testPool.ConnectManagers(setup.m_node.dmnman.get(), setup.m_node.llmq_ctx->isman.get());
    }
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    testPool.addUnchecked(entry.FromTx(tx_reg1));
    BOOST_CHECK_EQUAL(testPool.size(), 1U);
    BOOST_CHECK(testPool.existsProviderTxConflict(CTransaction(tx_reg2)));
}

// dash#7489: a ProRegTx that reuses a confirmed external collateral replaces
// the live MN at block connect. An update (ProUpServ/ProUpReg/ProUpRev) for the
// replaced proTxHash is still mempool-valid against the pre-block tip list, so
// fee-ordered packaging can put the replacement before the update and make
// BuildNewListFromBlock return bad-protx-hash, aborting block assembly. The
// mempool must treat the two as conflicts, whichever arrives first.
void FuncTestMempoolProRegReplacementUpdateConflict(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    int nHeight = chainman.ActiveChain().Height();
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;
    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Mine an external collateral, then register MN X against it.
    CMutableTransaction tx_collateral;
    FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, scriptCollateral,
                    dmn_types::BuildMnStruct(MnType::Regular).collat_amount, setup.coinbaseKey);
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);
    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_collateral}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(Assert(setup.m_node.chainman)->ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_REQUIRE_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);

    COutPoint collateralOutpoint;
    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::BuildMnStruct(MnType::Regular).collat_amount) {
            collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }
    BOOST_REQUIRE(!collateralOutpoint.hash.IsNull());

    auto make_external_reg = [&](int port, CKey& owner, const CKey& payout, CBLSSecretKey& op) {
        CProRegTx payload;
        payload.nVersion = CProRegTx::GetVersion(!bls::bls_legacy_scheme);
        payload.addr = LookupNumeric("1.1.1.1", port);
        payload.keyIDOwner = owner.GetPubKey().GetID();
        payload.pubKeyOperator.Set(op.GetPublicKey(), bls::bls_legacy_scheme.load());
        payload.keyIDVoting = owner.GetPubKey().GetID();
        payload.scriptPayout = GetScriptForDestination(PKHash(payout.GetPubKey()));
        payload.collateralOutpoint = collateralOutpoint;

        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_PROVIDER_REGISTER;
        FundTransaction(chainman.ActiveChain(), tx, utxos, payload.scriptPayout, 1 * COIN, setup.coinbaseKey);
        payload.inputsHash = CalcTxInputsHash(CTransaction(tx));
        CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
        SetTxPayload(tx, payload);
        SignTransaction(*(setup.m_node.mempool), tx, setup.coinbaseKey);
        return tx;
    };

    auto tx_reg = make_external_reg(/*port=*/1, ownerKey, payoutKey, operatorKey);
    const uint256 proTxHash = tx_reg.GetHash();
    block = std::make_shared<CBlock>(setup.CreateBlock({tx_reg}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(Assert(setup.m_node.chainman)->ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_REQUIRE_EQUAL(chainman.ActiveChain().Height(), nHeight + 2);
    BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(proTxHash));
    BOOST_REQUIRE(dmnman.GetListAtChainTip().GetMNByCollateral(collateralOutpoint) != nullptr);

    // Replacement ProRegTx reusing the same external collateral with fresh keys.
    CKey ownerKey2;
    CKey payoutKey2;
    CBLSSecretKey operatorKey2;
    ownerKey2.MakeNewKey(true);
    payoutKey2.MakeNewKey(true);
    operatorKey2.MakeNewKey();
    auto tx_reg_replace = make_external_reg(/*port=*/3, ownerKey2, payoutKey2, operatorKey2);

    // Update for the MN the replacement would delete.
    auto tx_up_serv = CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, proTxHash,
                                        operatorKey, /*port=*/2, CScript(), setup.coinbaseKey);

    CTxMemPool testPool;
    if (setup.m_node.dmnman) {
        testPool.ConnectManagers(setup.m_node.dmnman.get(), setup.m_node.llmq_ctx->isman.get());
    }
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    // Replacement already in mempool => update for the MN it replaces is a conflict.
    testPool.addUnchecked(entry.FromTx(tx_reg_replace));
    BOOST_CHECK_EQUAL(testPool.size(), 1U);
    BOOST_CHECK(testPool.existsProviderTxConflict(CTransaction(tx_up_serv)));
    testPool.removeRecursive(CTransaction(tx_reg_replace), MemPoolRemovalReason::MANUAL);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);

    // Update already in mempool => replacement ProRegTx reusing that MN's collateral conflicts.
    testPool.addUnchecked(entry.FromTx(tx_up_serv));
    BOOST_CHECK_EQUAL(testPool.size(), 1U);
    BOOST_CHECK(testPool.existsProviderTxConflict(CTransaction(tx_reg_replace)));

    // existsProviderTxConflict only gates our own acceptance; a miner can still confirm the
    // replacement. Once that block arrives, removeForBlock must evict the now-unmineable
    // update, or it lingers and stalls our own block assembly.
    std::vector<CTransactionRef> connected{MakeTransactionRef(tx_reg_replace)};
    testPool.removeForBlock(connected, chainman.ActiveChain().Height() + 1);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}

void FuncVerifyDB(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);

    int nHeight = chainman.ActiveChain().Height();
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Create a MN with an external collateral
    CMutableTransaction tx_collateral;
    FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, scriptCollateral, dmn_types::BuildMnStruct(MnType::Regular).collat_amount, setup.coinbaseKey);
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);

    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_collateral}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 1);
    BOOST_CHECK_EQUAL(block->GetHash(), chainman.ActiveChain().Tip()->GetBlockHash());

    CProRegTx payload;
    payload.nVersion = CProRegTx::GetVersion(!bls::bls_legacy_scheme);
    payload.addr = LookupNumeric("1.1.1.1", 1);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::BuildMnStruct(MnType::Regular).collat_amount) {
            payload.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = 3;
    tx_reg.nType = TRANSACTION_PROVIDER_REGISTER;
    FundTransaction(chainman.ActiveChain(), tx_reg, utxos, scriptPayout, dmn_types::BuildMnStruct(MnType::Regular).collat_amount, setup.coinbaseKey);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg, payload);
    SignTransaction(*(setup.m_node.mempool), tx_reg, setup.coinbaseKey);

    auto tx_reg_hash = tx_reg.GetHash();

    block = std::make_shared<CBlock>(setup.CreateBlock({tx_reg}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 2);
    BOOST_CHECK_EQUAL(block->GetHash(), chainman.ActiveChain().Tip()->GetBlockHash());
    BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx_reg_hash));

    // Now spend the collateral while updating the same MN
    SimpleUTXOMap collateral_utxos;
    collateral_utxos.emplace(payload.collateralOutpoint, std::make_pair(1, 1000));
    auto proUpRevTx = CreateProUpRevTx(chainman.ActiveChain(), *(setup.m_node.mempool), collateral_utxos, tx_reg_hash, operatorKey, collateralKey);

    // This fixture is about the list diff a collateral spend produces, not
    // about the maturity rule; mining 8,064 blocks to reach it would make the
    // test intractably slow. collateral_tests covers the rule itself.
    //
    // The suspension has to hold across the verification too, not just the
    // mining: VerifyDB at level 4 disconnects and reconnects the block, and
    // reconnecting runs the maturity check again. That it does is the point of
    // deriving the rule from the deterministic list -- a node reaches the same
    // verdict on a block whether it is connecting it for the first time or
    // replaying it -- so the fixture has to keep the exemption for both.
    ScopedCollateralMaturityOverride collateral_maturity_override{
        const_cast<Consensus::Params&>(Params().GetConsensus())};

    block = std::make_shared<CBlock>(setup.CreateBlock({proUpRevTx}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), nHeight + 3);
    BOOST_CHECK_EQUAL(block->GetHash(), chainman.ActiveChain().Tip()->GetBlockHash());
    BOOST_REQUIRE(!dmnman.GetListAtChainTip().HasMN(tx_reg_hash));

    // Verify db consistency
    LOCK(cs_main);
    BOOST_REQUIRE(CVerifyDB().VerifyDB(chainman.ActiveChainstate(), Params().GetConsensus(),
                                       chainman.ActiveChainstate().CoinsTip(), *(setup.m_node.evodb), 4, 2));
}

void FuncEvoDbDiffRoundTrip(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    // Keep the DIP3 activation block itself empty: the verifier anchors its
    // first snapshot at that height, and this test is about what follows it.
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);

    // Register a masternode, then update it in a later block. Verifying the
    // stored diffs must then carry state forward from one diff to the next:
    // the update names a masternode that exists only once the registration
    // diff has been applied. This is the exact shape that went undetected
    // while VerifySnapshotPair dropped ApplyDiff's return value.
    CKey owner_key;
    CBLSSecretKey operator_key;
    auto tx_reg = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1,
                                 GenerateRandomAddress(), setup.coinbaseKey, owner_key, operator_key);
    auto tx_reg_hash = tx_reg.GetHash();
    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_reg}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx_reg_hash));

    setup.CreateAndProcessBlock({}, setup.coinbaseKey);

    auto tx_upreg = CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, tx_reg_hash,
                                     owner_key, operator_key.GetPublicKey(), owner_key.GetPubKey().GetID(),
                                     GenerateRandomAddress(), setup.coinbaseKey);
    block = std::make_shared<CBlock>(setup.CreateBlock({tx_upreg}, setup.coinbaseKey, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(Params(), block, true, nullptr));
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    const uint256 update_block_hash = chainman.ActiveChain().Tip()->GetBlockHash();

    // Reach the first regular snapshot (DISK_SNAPSHOT_PERIOD, 576) so the
    // range holds one complete snapshot pair for the verifier to work on.
    while (chainman.ActiveChain().Height() <= 576) {
        setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    }
    dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());

    // repair=false never reaches the rebuild callback.
    CDeterministicMNManager::BuildListFromBlockFunc no_rebuild =
        [](const CBlock&, gsl::not_null<const CBlockIndex*>, const CDeterministicMNList&,
           const CCoinsViewCache&, bool, BlockValidationState&, CDeterministicMNList&) { return false; };

    auto result = dmnman.RecalculateAndRepairDiffs(chainman.ActiveChain().Genesis(), chainman.ActiveChain().Tip(),
                                                   chainman, no_rebuild, /*repair=*/false);
    const std::string first_error = result.verification_errors.empty() ? "" : result.verification_errors.front();
    BOOST_REQUIRE_MESSAGE(result.verification_errors.empty(), first_error);
    BOOST_CHECK_EQUAL(result.snapshots_verified, 1);

    // The pass above must not be vacuous: emptying one stored diff has to
    // break it. The key comes from the manager rather than a literal, because
    // a literal goes stale at the next format migration and then plants the
    // damage where nothing reads it -- which looks exactly like the verifier
    // missing real corruption.
    //
    // The corruption goes into the raw database, the layer repair writes to
    // and startup reads from -- a value planted in the uncommitted transaction
    // overlay would shadow the repair's writes and fail the test for reasons
    // production never sees. Drain the overlay first so raw is what Read hits.
    BOOST_REQUIRE(setup.m_node.evodb->CommitRootTransaction());
    const auto diff_key = std::make_pair(CDeterministicMNManager::ListDiffDbKey(), update_block_hash);
    // If this record is not there, the corruption below lands on nothing and
    // every check after it would pass for the wrong reason.
    BOOST_REQUIRE(setup.m_node.evodb->GetRawDB().Exists(diff_key));
    BOOST_REQUIRE(setup.m_node.evodb->GetRawDB().Write(diff_key, CDeterministicMNListDiff{}));
    result = dmnman.RecalculateAndRepairDiffs(chainman.ActiveChain().Genesis(), chainman.ActiveChain().Tip(),
                                              chainman, no_rebuild, /*repair=*/false);
    BOOST_CHECK(!result.verification_errors.empty());
    BOOST_CHECK_EQUAL(result.snapshots_verified, 0);

    // Repair must recalculate the damaged interval from blocks and accept its
    // own replay -- the replay is the second call site the discarded ApplyDiff
    // return value broke, and verify-only coverage never reaches it.
    CDeterministicMNManager::BuildListFromBlockFunc rebuild =
        [&](const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
            const CDeterministicMNList& prevList, const CCoinsViewCache& view,
            bool debugLogs, BlockValidationState& state, CDeterministicMNList& mnListRet) {
            return dmnman.RebuildListFromBlock(block, pindexPrev, prevList, state, view, mnListRet,
                                               *setup.m_node.llmq_ctx->qsnapman, debugLogs);
        };
    result = dmnman.RecalculateAndRepairDiffs(chainman.ActiveChain().Genesis(), chainman.ActiveChain().Tip(),
                                              chainman, rebuild, /*repair=*/true);
    const std::string first_repair_error = result.repair_errors.empty() ? "" : result.repair_errors.front();
    BOOST_REQUIRE_MESSAGE(result.repair_errors.empty(), first_repair_error);
    BOOST_CHECK(result.diffs_recalculated > 0);

    // And the database it leaves behind has to verify clean.
    result = dmnman.RecalculateAndRepairDiffs(chainman.ActiveChain().Genesis(), chainman.ActiveChain().Tip(),
                                              chainman, no_rebuild, /*repair=*/false);
    const std::string post_repair_error = result.verification_errors.empty() ? "" : result.verification_errors.front();
    BOOST_REQUIRE_MESSAGE(result.verification_errors.empty(), post_repair_error);
    BOOST_CHECK_EQUAL(result.snapshots_verified, 1);
    BOOST_CHECK_EQUAL(result.verified_through_height, 576);

    // The stretch past the last snapshot has no closing snapshot and used to
    // escape verification entirely. A diff there that cannot apply must fail
    // the walk now, while the pair section stays clean.
    CDeterministicMNListDiff poison;
    poison.updatedMNs.emplace(uint64_t{999999}, CDeterministicMNStateDiff{});
    const uint256 tip_hash = chainman.ActiveChain().Tip()->GetBlockHash();
    const auto tip_key = std::make_pair(CDeterministicMNManager::ListDiffDbKey(), tip_hash);
    BOOST_REQUIRE(setup.m_node.evodb->GetRawDB().Exists(tip_key));
    BOOST_REQUIRE(setup.m_node.evodb->GetRawDB().Write(tip_key, poison));
    result = dmnman.RecalculateAndRepairDiffs(chainman.ActiveChain().Genesis(), chainman.ActiveChain().Tip(),
                                              chainman, no_rebuild, /*repair=*/false);
    BOOST_CHECK(!result.verification_errors.empty());
    BOOST_CHECK_EQUAL(result.snapshots_verified, 1);
    BOOST_CHECK_EQUAL(result.verified_through_height, 576);
}

// dash 590236a2: PreVerifyMessage() bounded a justification's member index with
// `index > members.size()`, so the one-past-the-end index got through and
// ReceiveMessage() then read members[members.size()]. The last member must be
// accepted, and every index from members.size() on rejected with a ban. The
// justification is otherwise valid, so no earlier check decides the outcome.
void FuncDKGJustificationIndexBound(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto& llmq_ctx = *Assert(setup.m_node.llmq_ctx);
    const auto llmq_params = *Assert(Params().GetLLMQ(Consensus::LLMQType::LLMQ_TEST));

    // One masternode per quorum seat, one registration per block.
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    for (int i = 0; i < llmq_params.size; ++i) {
        CKey ownerKey;
        CBLSSecretKey operatorKey;
        auto tx = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, i + 1, GenerateRandomAddress(), setup.coinbaseKey, ownerKey, operatorKey);
        setup.CreateAndProcessBlock({tx}, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
    }
    // Up to the next quorum base block.
    do {
        setup.CreateAndProcessBlock({}, setup.coinbaseKey);
        dmnman.UpdatedBlockTip(chainman.ActiveChain().Tip());
    } while (chainman.ActiveChain().Height() % llmq_params.dkgInterval != 0);
    const CBlockIndex* base = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());

    // The session as an observer of that quorum.
    llmq::CDKGSession session(base, llmq_params, *llmq_ctx.bls_worker, dmnman, *llmq_ctx.qdkgsman,
                              *llmq_ctx.dkg_debugman, *Assert(setup.m_node.mn_metaman), *llmq_ctx.qsnapman,
                              /*mn_activeman=*/nullptr, *Assert(setup.m_node.sporkman));
    BOOST_REQUIRE(session.Init(/*_myProTxHash=*/uint256(), /*_quorumIndex=*/0));
    const auto members = llmq::utils::GetAllQuorumMembers(llmq_params.type, dmnman, *llmq_ctx.qsnapman, base);
    BOOST_REQUIRE_EQUAL(members.size(), size_t(llmq_params.size));

    const auto justification = [&](size_t index) {
        llmq::CDKGJustification qj;
        qj.llmqType = llmq_params.type;
        qj.quorumHash = base->GetBlockHash();
        qj.proTxHash = members[0]->proTxHash;
        CBLSSecretKey sk;
        sk.MakeNewKey();
        qj.contributions.push_back({static_cast<uint32_t>(index), sk});
        qj.sig = sk.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
        return qj;
    };

    bool ban{true};
    BOOST_CHECK(session.PreVerifyMessage(justification(members.size() - 1), ban));
    BOOST_CHECK(!ban);
    for (const size_t index : {members.size(), members.size() + 1}) {
        ban = false;
        BOOST_CHECK_MESSAGE(!session.PreVerifyMessage(justification(index), ban), "index " << index << " accepted");
        BOOST_CHECK_MESSAGE(ban, "index " << index << " not scored");
    }
}

// F-D31-MISSING-DIFF-BOUNDARY-01: GetListForBlockInternal took any block with
// neither a snapshot nor a readable diff for the point the list starts from.
// With one diff gone from the middle of the history, a manager starting cold
// returned an empty list at that height -- the registered masternode dropped --
// and cached it as real. Only the block before DIP0003 activation may lack
// both; above it the read has to fail, every time, and leave the intact
// history below the damage readable -- and a node starting on such a
// database has to stop while loading, not in the first thread that reads it.
void FuncMissingDiffIsNotListStart(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto& evodb = *Assert(setup.m_node.evodb);

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    auto tx_reg = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1,
                                 GenerateRandomAddress(), setup.coinbaseKey, owner_key, operator_key);
    const uint256 pro_tx_hash = tx_reg.GetHash();
    setup.CreateAndProcessBlock({tx_reg}, setup.coinbaseKey);
    const CBlockIndex* registered = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    const CBlockIndex* damaged = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());
    dmnman.UpdatedBlockTip(tip);
    BOOST_REQUIRE(damaged->pprev == registered && tip->pprev == damaged);
    BOOST_REQUIRE(dmnman.GetListForBlock(damaged).HasMN(pro_tx_hash));

    // The damage goes into the raw database, the layer a restarted node reads
    // from; drain the transaction overlay first so that is what the reads hit.
    BOOST_REQUIRE(evodb.CommitRootTransaction());
    const auto diff_key = [](const CBlockIndex* pindex) {
        return std::make_pair(CDeterministicMNManager::ListDiffDbKey(), pindex->GetBlockHash());
    };
    // A snapshot at or above a damaged height would answer before the damaged
    // diff is ever read, and every check below would pass vacuously.
    for (const CBlockIndex* pindex : {damaged, tip}) {
        BOOST_REQUIRE(evodb.GetRawDB().Exists(diff_key(pindex)));
        BOOST_REQUIRE(!evodb.GetRawDB().Exists(
            std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), pindex->GetBlockHash())));
    }

    // What a node starting on this database runs before any other reader:
    // LoadChainstate's check of the list at the tip, on a fresh manager.
    const auto load_check = [&]() -> std::string {
        CDeterministicMNManager fresh(chainman.ActiveChainstate(), evodb);
        try {
            return VerifyMasternodeListAtTip(tip, fresh) ? "accepted" : "refused";
        } catch (const std::exception& e) {
            return std::string{"threw: "} + e.what();
        }
    };

    // Intact history: a cold manager walks back to the real start of the list.
    {
        CDeterministicMNManager cold(chainman.ActiveChainstate(), evodb);
        BOOST_CHECK(cold.GetListForBlock(damaged).HasMN(pro_tx_hash));
        BOOST_CHECK(cold.GetListForBlock(tip).HasMN(pro_tx_hash));
    }
    BOOST_CHECK_EQUAL(load_check(), "accepted");

    // One damaged diff at a time: in the middle of the history, then at the
    // tip -- where the list used to come back empty without any error at all.
    for (const CBlockIndex* target : {damaged, tip}) {
        CDeterministicMNListDiff original;
        BOOST_REQUIRE(evodb.GetRawDB().Read(diff_key(target), original));
        for (const bool missing : {true, false}) {
            const std::string damage{strprintf("%s diff at height %d", missing ? "missing" : "undecodable", target->nHeight)};
            if (missing) {
                BOOST_REQUIRE(evodb.GetRawDB().Erase(diff_key(target)));
                BOOST_REQUIRE(!evodb.GetRawDB().Exists(diff_key(target)));
            } else {
                // A record that is there but cannot be decoded reads as false
                // too: a lone 0xfd announces a two-byte count that never follows.
                BOOST_REQUIRE(evodb.GetRawDB().Write(diff_key(target), uint8_t{0xfd}));
                BOOST_REQUIRE(evodb.GetRawDB().Exists(diff_key(target)));
            }
            CDeterministicMNManager cold(chainman.ActiveChainstate(), evodb);
            // Twice over: a list invented by the first read would answer the
            // second one from the cache.
            for (int attempt = 1; attempt <= 2; ++attempt) {
                for (const CBlockIndex* pindex : {damaged, tip}) {
                    if (pindex->nHeight < target->nHeight) continue;
                    try {
                        const auto list = cold.GetListForBlock(pindex);
                        BOOST_ERROR(damage << ": height " << pindex->nHeight << " returned a list of "
                                    << list.GetAllMNsCount() << " masternode(s) on attempt " << attempt);
                    } catch (const dbwrapper_error&) {
                    } catch (const std::exception& e) {
                        BOOST_ERROR(damage << ": height " << pindex->nHeight << " failed on attempt " << attempt
                                    << " with something other than a database error: " << e.what());
                    }
                }
            }
            // Below the damage the history is intact and still reads.
            BOOST_CHECK_MESSAGE(cold.GetListForBlock(target->pprev).HasMN(pro_tx_hash), damage);
            // And the node stops at load time, where the error reaches the user.
            const std::string loaded{load_check()};
            BOOST_CHECK_MESSAGE(loaded == "refused", damage << ": load-time check " << loaded);
        }
        BOOST_REQUIRE(evodb.GetRawDB().Write(diff_key(target), original));
    }
    BOOST_CHECK_EQUAL(load_check(), "accepted");
}

// The recovery path for the same damage between two intact snapshots: the tip
// still reads from the later snapshot, so the load check passes, while an
// older list -- a quorum's work block, as the import reads for its quorum
// connections -- fails with a database error. Verification reports it, a
// repair that fails in storage leaves it as it was without an exception, and
// the repair -evodbrepair asks for (VerifyOrRepairDiffs, before the import)
// rebuilds the diff from blocks, after which that list reads exactly as before.
void FuncRepairableDiffDamage(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto& evodb = *Assert(setup.m_node.evodb);
    auto& qsnapman = *Assert(setup.m_node.llmq_ctx->qsnapman);

    // Keep the DIP3 activation block empty: the verifier anchors its first
    // snapshot pair there (see FuncEvoDbDiffRoundTrip).
    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    auto tx_reg = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1,
                                 GenerateRandomAddress(), setup.coinbaseKey, owner_key, operator_key);
    const uint256 pro_tx_hash = tx_reg.GetHash();
    setup.CreateAndProcessBlock({tx_reg}, setup.coinbaseKey);
    // Past the first regular snapshot (DISK_SNAPSHOT_PERIOD, 576), so the tip
    // list starts there and never reads the damaged diff below it.
    while (chainman.ActiveChain().Height() <= 580) {
        setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    }
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());
    dmnman.UpdatedBlockTip(tip);
    const CBlockIndex* damaged = tip->GetAncestor(440);
    const CBlockIndex* work = tip->GetAncestor(448);
    const CBlockIndex* snapshot = tip->GetAncestor(576);
    const auto reference = dmnman.GetListForBlock(work);
    BOOST_REQUIRE(reference.HasMN(pro_tx_hash));

    BOOST_REQUIRE(evodb.CommitRootTransaction());
    const auto diff_key = std::make_pair(CDeterministicMNManager::ListDiffDbKey(), damaged->GetBlockHash());
    BOOST_REQUIRE(evodb.GetRawDB().Exists(diff_key));
    BOOST_REQUIRE(evodb.GetRawDB().Exists(
        std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), snapshot->GetBlockHash())));
    for (const CBlockIndex* pindex : {damaged, work}) {
        BOOST_REQUIRE(!evodb.GetRawDB().Exists(
            std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), pindex->GetBlockHash())));
    }

    for (const bool missing : {true, false}) {
        const std::string damage{strprintf("%s diff at height %d", missing ? "missing" : "undecodable", damaged->nHeight)};
        if (missing) {
            BOOST_REQUIRE(evodb.GetRawDB().Erase(diff_key));
        } else {
            BOOST_REQUIRE(evodb.GetRawDB().Write(diff_key, uint8_t{0xfd}));
        }
        {
            CDeterministicMNManager cold(chainman.ActiveChainstate(), evodb);
            BOOST_CHECK_MESSAGE(VerifyMasternodeListAtTip(tip, cold), damage << ": the tip list did not load");
            try {
                const auto list = cold.GetListForBlock(work);
                BOOST_ERROR(damage << ": the work block returned a list of " << list.GetAllMNsCount() << " masternode(s)");
            } catch (const dbwrapper_error&) {
            } catch (const std::exception& e) {
                BOOST_ERROR(damage << ": the work block failed with something other than a database error: " << e.what());
            }
        }

        // What startup runs: verification in the background, the repair before
        // anything else when -evodbrepair asks for it.
        using Outcome = CDeterministicMNManager::HistoryCheck::Outcome;
        const auto work_still_damaged = [&]() {
            CDeterministicMNManager cold(chainman.ActiveChainstate(), evodb);
            try {
                (void)cold.GetListForBlock(work);
            } catch (const dbwrapper_error&) {
                return true;
            }
            return false;
        };
        CDeterministicMNManager repairer(chainman.ActiveChainstate(), evodb);
        CDeterministicMNManager::BuildListFromBlockFunc rebuild =
            [&](const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
                const CDeterministicMNList& prevList, const CCoinsViewCache& view,
                bool debugLogs, BlockValidationState& state, CDeterministicMNList& mnListRet) {
                return repairer.RebuildListFromBlock(block, pindexPrev, prevList, state, view, mnListRet,
                                                     qsnapman, debugLogs);
            };
        // Verification alone reports the damage and writes nothing.
        auto check = repairer.VerifyOrRepairDiffs(chainman, rebuild, /*repair=*/false);
        BOOST_CHECK_MESSAGE(check.outcome == Outcome::UNRELIABLE, damage << ": verification outcome "
                            << static_cast<int>(check.outcome));
        BOOST_CHECK_MESSAGE(work_still_damaged(), damage << ": verification changed the history");
        // A repair whose block replay fails in storage ends as an outcome the
        // caller stops on -- not an exception, not a success -- and writes nothing.
        CDeterministicMNManager::BuildListFromBlockFunc failing =
            [](const CBlock&, gsl::not_null<const CBlockIndex*>, const CDeterministicMNList&, const CCoinsViewCache&,
               bool, BlockValidationState&, CDeterministicMNList&) -> bool {
                throw dbwrapper_error("injected replay failure");
            };
        try {
            check = repairer.VerifyOrRepairDiffs(chainman, failing, /*repair=*/true);
            BOOST_CHECK_MESSAGE(check.outcome == Outcome::REPAIR_FAILED || check.outcome == Outcome::FAILED,
                                damage << ": failed repair outcome " << static_cast<int>(check.outcome));
            BOOST_CHECK_MESSAGE(check.details.find("injected replay failure") != std::string::npos,
                                damage << ": failed repair details " << check.details);
        } catch (const std::exception& e) {
            BOOST_ERROR(damage << ": the failed repair escaped as an exception: " << e.what());
        }
        BOOST_CHECK_MESSAGE(work_still_damaged(), damage << ": the failed repair changed the history");
        // The repair -evodbrepair asks for rebuilds the diff and marks the history.
        check = repairer.VerifyOrRepairDiffs(chainman, rebuild, /*repair=*/true);
        BOOST_CHECK_MESSAGE(check.outcome == Outcome::CLEAN, damage << ": repair outcome "
                            << static_cast<int>(check.outcome) << " " << check.details);
        BOOST_CHECK(repairer.IsRepaired());

        CDeterministicMNManager restarted(chainman.ActiveChainstate(), evodb);
        BOOST_CHECK(VerifyMasternodeListAtTip(tip, restarted));
        try {
            const auto list = restarted.GetListForBlock(work);
            BOOST_CHECK_MESSAGE(!reference.BuildDiff(list).HasChanges(), damage << ": the repaired list differs");
        } catch (const std::exception& e) {
            BOOST_ERROR(damage << ": the repaired work block still fails: " << e.what());
        }
    }
}

// The same damage behind the same intact snapshot, reached the way a peer
// reaches it: a request for an older list than anything startup reads -- here a
// list diff whose base lies between the damaged diff and the snapshot the tip
// list starts from. The message handler logged the database error as one more
// bad message and kept the node running on the damaged history; it has to stop
// the node in order, as the block import does, and still answer a request the
// intact history can serve.
void FuncDamagedHistoryStopsMessageHandling(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& evodb = *Assert(setup.m_node.evodb);

    setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    auto tx_reg = CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1,
                                 GenerateRandomAddress(), setup.coinbaseKey, owner_key, operator_key);
    setup.CreateAndProcessBlock({tx_reg}, setup.coinbaseKey);
    while (chainman.ActiveChain().Height() <= 580) {
        setup.CreateAndProcessBlock({}, setup.coinbaseKey);
    }
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());
    const CBlockIndex* damaged = tip->GetAncestor(440);
    const CBlockIndex* base = tip->GetAncestor(448);
    const CBlockIndex* snapshot = tip->GetAncestor(576);

    BOOST_REQUIRE(evodb.CommitRootTransaction());
    const auto diff_key = std::make_pair(CDeterministicMNManager::ListDiffDbKey(), damaged->GetBlockHash());
    BOOST_REQUIRE(evodb.GetRawDB().Exists(diff_key));
    BOOST_REQUIRE(evodb.GetRawDB().Exists(
        std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), snapshot->GetBlockHash())));
    for (const CBlockIndex* pindex : {damaged, base}) {
        BOOST_REQUIRE(!evodb.GetRawDB().Exists(
            std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), pindex->GetBlockHash())));
    }

    // The handler reads through m_node.dmnman, whose cache holds every diff it
    // wrote; a manager starting cold reads them from disk, as after a restart.
    // The rest of the node still refers to the live one, which goes back
    // however the test ends.
    struct RestoreManager {
        std::unique_ptr<CDeterministicMNManager>& slot;
        std::unique_ptr<CDeterministicMNManager> live;
        ~RestoreManager() { slot = std::move(live); }
    } restore{setup.m_node.dmnman,
              std::exchange(setup.m_node.dmnman,
                            std::make_unique<CDeterministicMNManager>(chainman.ActiveChainstate(), evodb))};
    // The abort requests a shutdown, which writes to the shutdown pipe.
    BOOST_REQUIRE(InitShutdownState());

    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        auto& connman = static_cast<ConnmanTestMsg&>(*setup.m_node.connman);
        auto& peerman = *setup.m_node.peerman;
        CNode peer{/*id=*/0,
                   /*sock=*/nullptr,
                   CAddress{},
                   /*nKeyedNetGroupIn=*/0,
                   /*nLocalHostNonceIn=*/0,
                   CAddress{},
                   /*addrNameIn=*/"",
                   ConnectionType::INBOUND,
                   /*inbound_onion=*/false};
        connman.Handshake(peer, /*successfully_connected=*/true, ServiceFlags(NODE_NETWORK),
                          ServiceFlags(NODE_NETWORK), PROTOCOL_VERSION, /*relay_txs=*/true);
        BOOST_REQUIRE(!peer.fDisconnect);

        // Returns whether the request was answered with a list diff.
        const auto request_diff = [&]() {
            connman.FlushSendBuffer(peer);
            CGetSimplifiedMNListDiff request;
            request.baseBlockHash = base->GetBlockHash();
            request.blockHash = tip->GetBlockHash();
            (void)connman.ReceiveMsgFrom(peer, CNetMsgMaker{peer.GetCommonVersion()}.Make(NetMsgType::GETMNLISTDIFF, request));
            peer.fPauseSend = false;
            connman.ProcessMessagesOnce(peer);
            LOCK(peer.cs_vSend);
            return std::any_of(peer.vSendMsg.begin(), peer.vSendMsg.end(),
                               [](const CSerializedNetMsg& msg) { return msg.m_type == NetMsgType::MNLISTDIFF; });
        };

        BOOST_CHECK_MESSAGE(request_diff(), "the intact history did not answer the request");
        BOOST_CHECK(!ShutdownRequested());

        for (const bool missing : {true, false}) {
            const std::string damage{strprintf("%s diff at height %d", missing ? "missing" : "undecodable", damaged->nHeight)};
            if (missing) {
                BOOST_REQUIRE(evodb.GetRawDB().Erase(diff_key));
            } else {
                BOOST_REQUIRE(evodb.GetRawDB().Write(diff_key, uint8_t{0xfd}));
            }
            setup.m_node.dmnman = std::make_unique<CDeterministicMNManager>(chainman.ActiveChainstate(), evodb);

            BOOST_CHECK_MESSAGE(!request_diff(), damage << ": the damaged history answered the request");
            BOOST_CHECK_MESSAGE(ShutdownRequested(), damage << ": the node kept running");
            const std::string warning{GetWarnings(/*verbose=*/true).original};
            BOOST_CHECK_MESSAGE(warning.find(strprintf("Masternode list diff at height %d", damaged->nHeight)) != std::string::npos,
                                damage << ": the abort does not name the damaged diff: " << warning);
            BOOST_CHECK_MESSAGE(warning.find("-reindex") != std::string::npos,
                                damage << ": the abort does not name the way out: " << warning);
            AbortShutdown();
            SetMiscWarning(Untranslated(""));
        }

        peerman.FinalizeNode(peer);
    }
}

BOOST_AUTO_TEST_SUITE(evo_dip3_activation_tests)

struct TestChainDIP3BeforeActivationSetup : public TestChainSetup {
    TestChainDIP3BeforeActivationSetup() :
        TestChainSetup(430)
    {
    }
};

struct TestChainDIP3Setup : public TestChainDIP3BeforeActivationSetup {
    TestChainDIP3Setup()
    {
        // Activate DIP3 here
        CreateAndProcessBlock({}, coinbaseKey);
    }
};

struct TestChainV19BeforeActivationSetup : public TestChainSetup {
    TestChainV19BeforeActivationSetup();
};

struct TestChainV19Setup : public TestChainV19BeforeActivationSetup {
    TestChainV19Setup()
    {
        // Activate V19
        for (int i = 0; i < 5; ++i) {
            CreateAndProcessBlock({}, coinbaseKey);
        }
        bool v19_just_activated{DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), Params().GetConsensus(),
                                                      Consensus::DEPLOYMENT_V19) &&
                                !DeploymentActiveAt(*m_node.chainman->ActiveChain().Tip(), Params().GetConsensus(),
                                                    Consensus::DEPLOYMENT_V19)};
        assert(v19_just_activated);
    }
};

// 5 blocks earlier
TestChainV19BeforeActivationSetup::TestChainV19BeforeActivationSetup() :
    TestChainSetup(494, {"-testactivationheight=v19@500"})
{
    bool v19_active{DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), Params().GetConsensus(),
                                          Consensus::DEPLOYMENT_V19)};
    assert(!v19_active);
}

// DIP3 can only be activated with legacy scheme (v19 is activated later)
BOOST_AUTO_TEST_CASE(dip3_activation_legacy)
{
    TestChainDIP3BeforeActivationSetup setup;
    FuncDIP3Activation(setup);
}

// V19 can only be activated with legacy scheme
BOOST_AUTO_TEST_CASE(v19_activation_legacy)
{
    TestChainV19BeforeActivationSetup setup;
    FuncV19Activation(setup);
}

BOOST_AUTO_TEST_CASE(v19_boundary_validation_failure_restores_bls_scheme)
{
    TestChainV19Setup setup;
    auto& chainman = *Assert(setup.m_node.chainman);
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());

    BOOST_REQUIRE(!DeploymentActiveAt(*chainman.ActiveChain().Tip(), Params().GetConsensus(),
                                      Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(DeploymentActiveAfter(chainman.ActiveChain().Tip(), Params().GetConsensus(),
                                        Consensus::DEPLOYMENT_V19));

    struct ScopedBLSLegacySchemeRestore {
        explicit ScopedBLSLegacySchemeRestore(bool saved_scheme) : m_saved_scheme(saved_scheme) {}
        ~ScopedBLSLegacySchemeRestore() { bls::bls_legacy_scheme.store(m_saved_scheme); }
        const bool m_saved_scheme;
    } bls_scheme_restore{bls::bls_legacy_scheme.load()};

    CMutableTransaction bad_tx;
    bad_tx.nVersion = 1;
    bad_tx.vin.emplace_back(COutPoint(uint256::ONE, 0));
    bad_tx.vout.emplace_back(1 * COIN, CScript{} << OP_TRUE);

    bls::bls_legacy_scheme.store(true);

    CBlock proposal_block = setup.CreateBlock({bad_tx}, coinbase_pk, chainman.ActiveChainstate());
    {
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_CHECK(!TestBlockValidity(state, *Assert(setup.m_node.llmq_ctx)->clhandler, *Assert(setup.m_node.evodb),
                                       Params(), chainman.ActiveChainstate(), proposal_block,
                                       chainman.ActiveChain().Tip(), /*fCheckPOW=*/true,
                                       /*fCheckMerkleRoot=*/true));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
    BOOST_CHECK(bls::bls_legacy_scheme.load());

    CBlock connect_block = setup.CreateBlock({bad_tx}, coinbase_pk, chainman.ActiveChainstate());
    const int height_before_invalid_block{chainman.ActiveChain().Height()};
    (void)chainman.ProcessNewBlock(Params(), std::make_shared<const CBlock>(connect_block),
                                   /*force_processing=*/true, /*new_block=*/nullptr);
    BOOST_CHECK_EQUAL(chainman.ActiveChain().Height(), height_before_invalid_block);
    BOOST_CHECK(bls::bls_legacy_scheme.load());
}

BOOST_AUTO_TEST_CASE(dip3_protx_legacy)
{
    TestChainDIP3Setup setup;
    FuncDIP3Protx(setup);
}

BOOST_AUTO_TEST_CASE(dip3_protx_basic)
{
    TestChainV19Setup setup;
    FuncDIP3Protx(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_reorg_legacy)
{
    TestChainDIP3Setup setup;
    FuncTestMempoolReorg(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_reorg_basic)
{
    TestChainV19Setup setup;
    FuncTestMempoolReorg(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_protx_key_changed_conflict_chain_legacy)
{
    TestChainDIP3Setup setup;
    FuncTestMempoolProTxKeyChangedConflictChain(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_protx_key_changed_conflict_chain_basic)
{
    TestChainV19Setup setup;
    FuncTestMempoolProTxKeyChangedConflictChain(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_dual_proregtx_legacy)
{
    TestChainDIP3Setup setup;
    FuncTestMempoolDualProregtx(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_dual_proregtx_basic)
{
    TestChainV19Setup setup;
    FuncTestMempoolDualProregtx(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_proreg_replacement_update_conflict)
{
    TestChainV19Setup setup;
    FuncTestMempoolProRegReplacementUpdateConflict(setup);
}

//This one can be started only with legacy scheme, since inside undo block will switch it back to legacy resulting into an inconsistency
BOOST_AUTO_TEST_CASE(verify_db_legacy)
{
    TestChainDIP3Setup setup;
    FuncVerifyDB(setup);
}

BOOST_AUTO_TEST_CASE(evodb_diff_round_trip)
{
    TestChainDIP3Setup setup;
    FuncEvoDbDiffRoundTrip(setup);
}

BOOST_AUTO_TEST_CASE(dkg_justification_index_bound)
{
    TestChainDIP3Setup setup;
    FuncDKGJustificationIndexBound(setup);
}

BOOST_AUTO_TEST_CASE(missing_diff_is_not_list_start)
{
    TestChainDIP3Setup setup;
    FuncMissingDiffIsNotListStart(setup);
}

BOOST_AUTO_TEST_CASE(repairable_diff_damage)
{
    TestChainDIP3Setup setup;
    FuncRepairableDiffDamage(setup);
}

BOOST_AUTO_TEST_CASE(damaged_history_stops_message_handling)
{
    TestChainDIP3Setup setup;
    FuncDamagedHistoryStopsMessageHandling(setup);
}

BOOST_AUTO_TEST_SUITE_END()
