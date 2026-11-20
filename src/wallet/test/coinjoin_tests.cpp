// Copyright (c) 2020-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <coinjoin/client.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/context.h>
#include <coinjoin/options.h>
#include <coinjoin/util.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <interfaces/coinjoin.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <llmq/context.h>
#include <llmq/instantsend.h>
#include <logging.h>
#include <node/context.h>
#include <chainparams.h>
#include <evo/evodb.h>
#include <llmq/chainlocks.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <spork.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <policy/settings.h>
#include <pow.h>
#include <validation.h>
#include <txmempool.h>
#include <consensus/validation.h>
#include <wallet/coincontrol.h>
#include <wallet/walletutil.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace llmq {
// Defined in instantsend_queue_tests.cpp, which holds the manager's test seam: stores a lock as a verified one.
void StoreInstantSendLockForTest(CInstantSendManager& manager, const CInstantSendLock& islock);
} // namespace llmq

// The client session's test seams (friends of CCoinJoinClientSession): the ready step's selection and the whole ready
// step with a given session budget, and one dry PrepareDenominate over a range of round counts.
bool SelectDenominateForTest(CCoinJoinClientSession& session, CAmount nBudget, std::vector<CTxDSIn>& vecTxDSInRet)
{
    session.m_session_budget = nBudget;
    LOCK(session.m_wallet->cs_wallet);
    std::string strError;
    return session.SelectDenominate(strError, vecTxDSInRet);
}

bool SubmitDenominateForTest(CCoinJoinClientSession& session, CAmount nBudget, CConnman& connman)
{
    session.m_session_budget = nBudget;
    return session.SubmitDenominate(connman);
}

bool PrepareDenominateForTest(CCoinJoinClientSession& session, int nMinRounds, int nMaxRounds,
                              const std::vector<CTxDSIn>& vecTxDSIn,
                              std::vector<std::pair<CTxDSIn, CTxOut>>& vecPSInOutPairsRet)
{
    LOCK(session.m_wallet->cs_wallet);
    std::string strError;
    return session.PrepareDenominate(nMinRounds, nMaxRounds, strError, vecTxDSIn, vecPSInOutPairsRet, /*fDryRun=*/true);
}

BOOST_FIXTURE_TEST_SUITE(coinjoin_tests, BasicTestingSetup)

BOOST_FIXTURE_TEST_CASE(coinjoin_options_tests, TestingSetup)
{
    gArgs.ForceSetArg("-enablecoinjoin", "0");
    const auto loader = interfaces::MakeCoinJoinLoader(*m_node.cj_ctx->walletman);

    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetSessions(), DEFAULT_COINJOIN_SESSIONS);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetRounds(), DEFAULT_COINJOIN_ROUNDS);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetRandomRounds(), COINJOIN_RANDOM_ROUNDS);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetAmount(), DEFAULT_COINJOIN_AMOUNT);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetDenomsGoal(), DEFAULT_COINJOIN_DENOMS_GOAL);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetDenomsHardCap(), DEFAULT_COINJOIN_DENOMS_HARDCAP);

    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::IsEnabled(), false);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::IsMultiSessionEnabled(), DEFAULT_COINJOIN_MULTISESSION);

    CCoinJoinClientOptions::SetEnabled(true);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::IsEnabled(), true);
    CCoinJoinClientOptions::SetEnabled(false);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::IsEnabled(), false);

    CCoinJoinClientOptions::SetMultiSessionEnabled(!DEFAULT_COINJOIN_MULTISESSION);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::IsMultiSessionEnabled(), !DEFAULT_COINJOIN_MULTISESSION);
    CCoinJoinClientOptions::SetMultiSessionEnabled(DEFAULT_COINJOIN_MULTISESSION);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::IsMultiSessionEnabled(), DEFAULT_COINJOIN_MULTISESSION);

    CCoinJoinClientOptions::SetRounds(DEFAULT_COINJOIN_ROUNDS + 10);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetRounds(), DEFAULT_COINJOIN_ROUNDS + 10);
    CCoinJoinClientOptions::SetAmount(DEFAULT_COINJOIN_AMOUNT + 50);
    BOOST_CHECK_EQUAL(CCoinJoinClientOptions::GetAmount(), DEFAULT_COINJOIN_AMOUNT + 50);
}

BOOST_AUTO_TEST_CASE(coinjoin_collateral_tests)
{
    // Good collateral values
    static_assert(CoinJoin::IsCollateralAmount(0.00010000 * COIN));
    static_assert(CoinJoin::IsCollateralAmount(0.00012345 * COIN));
    static_assert(CoinJoin::IsCollateralAmount(0.00032123 * COIN));
    static_assert(CoinJoin::IsCollateralAmount(0.00019000 * COIN));

    // Bad collateral values
    static_assert(!CoinJoin::IsCollateralAmount(0.00009999 * COIN));
    static_assert(!CoinJoin::IsCollateralAmount(0.00040001 * COIN));
    static_assert(!CoinJoin::IsCollateralAmount(0.00100000 * COIN));
    static_assert(!CoinJoin::IsCollateralAmount(0.00100001 * COIN));
}

BOOST_AUTO_TEST_CASE(coinjoin_pending_dsa_request_tests)
{
    CPendingDsaRequest dsa_request;
    BOOST_CHECK(dsa_request.GetAddr() == CService());
    BOOST_CHECK(dsa_request.GetDSA() == CCoinJoinAccept());
    BOOST_CHECK_EQUAL(dsa_request.IsExpired(), true);
    CPendingDsaRequest dsa_request_2;
    BOOST_CHECK(dsa_request == dsa_request_2);
    CCoinJoinAccept cja;
    cja.nDenom = 4;
    CService cserv(CNetAddr(), 1111);
    CPendingDsaRequest custom_request(cserv, cja);
    BOOST_CHECK(custom_request.GetAddr() == cserv);
    BOOST_CHECK(custom_request.GetDSA() == cja);
    BOOST_CHECK_EQUAL(custom_request.IsExpired(), false);
    SetMockTime(GetTime() + 15);
    BOOST_CHECK_EQUAL(custom_request.IsExpired(), false);
    SetMockTime(GetTime() + 1);
    BOOST_CHECK_EQUAL(custom_request.IsExpired(), true);

    BOOST_CHECK(dsa_request != custom_request);
    BOOST_CHECK(!(dsa_request == custom_request));
    BOOST_CHECK(!dsa_request);
    BOOST_CHECK(custom_request);
}

BOOST_AUTO_TEST_CASE(coinjoin_dstxin_tests)
{
    CTxDSIn txin;
    BOOST_CHECK(txin.prevPubKey == CScript());
    BOOST_CHECK_EQUAL(txin.fHasSig, false);
    BOOST_CHECK_EQUAL(txin.nRounds, -10);
    CTxDSIn custom_txin(txin, CScript(4), -9);
    BOOST_CHECK(custom_txin.prevPubKey == CScript(4));
    BOOST_CHECK_EQUAL(custom_txin.fHasSig, false);
    BOOST_CHECK_EQUAL(custom_txin.nRounds, -9);
}

BOOST_AUTO_TEST_CASE(coinjoin_status_update_tests)
{
    CCoinJoinStatusUpdate cjsu;
    BOOST_CHECK_EQUAL(cjsu.nSessionID, 0);
    BOOST_CHECK_EQUAL(cjsu.nState, POOL_STATE_IDLE);
    BOOST_CHECK_EQUAL(cjsu.nEntriesCount, 0);
    BOOST_CHECK_EQUAL(cjsu.nStatusUpdate, STATUS_ACCEPTED);
    BOOST_CHECK_EQUAL(cjsu.nMessageID, MSG_NOERR);
    CCoinJoinStatusUpdate custom_cjsu(1, POOL_STATE_QUEUE, 1, STATUS_REJECTED, ERR_QUEUE_FULL);
    BOOST_CHECK_EQUAL(custom_cjsu.nSessionID, 1);
    BOOST_CHECK_EQUAL(custom_cjsu.nState, POOL_STATE_QUEUE);
    BOOST_CHECK_EQUAL(custom_cjsu.nEntriesCount, 1);
    BOOST_CHECK_EQUAL(custom_cjsu.nStatusUpdate, STATUS_REJECTED);
    BOOST_CHECK_EQUAL(custom_cjsu.nMessageID, ERR_QUEUE_FULL);
}

BOOST_AUTO_TEST_CASE(coinjoin_accept_tests)
{
    CCoinJoinAccept cja;
    BOOST_CHECK_EQUAL(cja.nDenom, 0);
    BOOST_CHECK_EQUAL(cja.txCollateral.GetHash(), CMutableTransaction().GetHash());
    // CMutableTransaction custom_cmt()
}

class CTransactionBuilderTestSetup : public TestChain100Setup
{
public:
    CTransactionBuilderTestSetup()
    {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        wallet = std::make_unique<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "", CreateMockWalletDatabase());
        wallet->SetupLegacyScriptPubKeyMan();
        wallet->LoadWallet();
        AddWallet(wallet);
        {
            LOCK2(wallet->cs_wallet, cs_main);
            wallet->GetLegacyScriptPubKeyMan()->AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
            wallet->SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet->ScanForWalletTransactions(/*start_block=*/wallet->chain().getBlockHash(0),
                                                                       /*start_height=*/0, /*max_height=*/{}, reserver,
                                                                       /*fUpdate=*/true);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
    }

    ~CTransactionBuilderTestSetup()
    {
        RemoveWallet(wallet, std::nullopt);
    }

    std::shared_ptr<CWallet> wallet;

    CWalletTx& AddTxToChain(uint256 nTxHash)
    {
        std::map<uint256, CWalletTx>::iterator it;
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            it = wallet->mapWallet.find(nTxHash);
            assert(it != wallet->mapWallet.end());
            blocktx = CMutableTransaction(*it->second.tx);
        }
        CreateAndProcessBlock({blocktx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        LOCK2(wallet->cs_wallet, cs_main);
        wallet->SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        CWalletTx::Confirmation confirm(CWalletTx::Status::CONFIRMED, m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash(), 1);
        it->second.m_confirm = confirm;
        return it->second;
    }
    // One transaction from the wallet's non-denominated coins paying `outputs`; mined when `confirm`, else accepted into
    // the mempool (this wallet does not broadcast) and seen there by the wallet.
    CTransactionRef PayOutputs(const std::vector<std::pair<CScript, CAmount>>& outputs, bool confirm)
    {
        std::vector<CRecipient> recipients;
        for (const auto& [script, amount] : outputs) recipients.push_back({script, amount, false});
        CTransactionRef tx;
        CAmount fee;
        int change_pos{-1};
        bilingual_str error;
        CCoinControl coin_control;
        coin_control.m_feerate = CFeeRate(1000);
        coin_control.nCoinType = CoinType::ONLY_NONDENOMINATED;
        FeeCalculation fee_calc;
        BOOST_REQUIRE(wallet->CreateTransaction(recipients, tx, fee, change_pos, error, coin_control, fee_calc));
        {
            LOCK2(wallet->cs_wallet, cs_main);
            wallet->CommitTransaction(tx, {}, {});
        }
        if (confirm) {
            Confirm(tx);
        } else {
            BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ProcessTransaction(tx).m_result_type) ==
                          MempoolAcceptResult::ResultType::VALID);
            // This wallet is not subscribed to the chain's notifications (no CWallet::Create): tell it, as they would.
            wallet->transactionAddedToMempool(tx, /*nAcceptTime=*/GetTime(), /*mempool_sequence=*/0);
            LOCK(wallet->cs_wallet);
            const CWalletTx* wtx{wallet->GetWalletTx(tx->GetHash())};
            BOOST_REQUIRE(wtx && wtx->InMempool() && wtx->GetDepthInMainChain() == 0 && !wtx->IsLockedByInstantSend());
        }
        return tx;
    }

    // Mines the wallet's transaction through AddTxToChain, after checking that its block became the tip (the helper
    // marks the transaction confirmed by the tip it finds) and that the transaction's first output is in the UTXO set.
    void Confirm(const CTransactionRef& tx)
    {
        const int height{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height())};
        AddTxToChain(tx->GetHash());
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(m_node.chainman->ActiveChain().Height(), height + 1);
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().CoinsTip().HaveCoin(COutPoint{tx->GetHash(), 0}));
    }

    // A zero-fee, all-denominated spend of one ready coin to `script`: its output has one round more than the coin.
    COutPoint MixOnce(const COutPoint& coin, CAmount amount, const CScript& script)
    {
        CMutableTransaction mtx;
        mtx.vin.emplace_back(coin);
        mtx.vout.emplace_back(amount, script);
        {
            LOCK(wallet->cs_wallet);
            BOOST_REQUIRE(wallet->SignTransaction(mtx));
        }
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        {
            LOCK2(wallet->cs_wallet, cs_main);
            wallet->CommitTransaction(tx, {}, {});
        }
        Confirm(tx);
        return COutPoint{tx->GetHash(), 0};
    }

    // A wallet of the test's own learns the chain's coins: the tip is its last processed block, then a full rescan.
    void Rescan(CWallet& w)
    {
        {
            LOCK2(w.cs_wallet, cs_main);
            w.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        WalletRescanReserver reserver(w);
        BOOST_REQUIRE(reserver.reserve());
        BOOST_REQUIRE_EQUAL(w.ScanForWalletTransactions(w.chain().getBlockHash(0), /*start_height=*/0, /*max_height=*/{},
                                                        reserver, /*fUpdate=*/true).status,
                            CWallet::ScanResult::SUCCESS);
    }

    // The ready-to-mix coins AvailableCoins lists (spent, locked and fully mixed coins and other amounts left out), with
    // whether each counts as spendable.
    static std::map<COutPoint, bool> ReadyToMix(CWallet& w)
    {
        std::vector<COutput> coins;
        CCoinControl coin_control;
        coin_control.nCoinType = CoinType::ONLY_READY_TO_MIX;
        LOCK(w.cs_wallet);
        w.AvailableCoins(coins, &coin_control);
        std::map<COutPoint, bool> outpoints;
        for (const auto& out : coins) outpoints.emplace(COutPoint{out.tx->GetHash(), static_cast<uint32_t>(out.i)}, out.fSpendable);
        return outpoints;
    }

    CompactTallyItem GetTallyItem(const std::vector<CAmount>& vecAmounts)
    {
        CompactTallyItem tallyItem;
        ReserveDestination reserveDest(wallet.get());
        CAmount nFeeRet;
        int nChangePosRet = -1;
        bilingual_str strError;
        CCoinControl coinControl;
        coinControl.m_feerate = CFeeRate(1000);
        {
            LOCK(wallet->cs_wallet);
            BOOST_CHECK(reserveDest.GetReservedDestination(tallyItem.txdest, false));
        }
        for (CAmount nAmount : vecAmounts) {
            CTransactionRef tx;
            FeeCalculation fee_calc_out;
            BOOST_CHECK(wallet->CreateTransaction({{GetScriptForDestination(tallyItem.txdest), nAmount, false}}, tx, nFeeRet, nChangePosRet, strError, coinControl, fee_calc_out));
            {
                LOCK2(wallet->cs_wallet, cs_main);
                wallet->CommitTransaction(tx, {}, {});
            }
            AddTxToChain(tx->GetHash());
            for (size_t n = 0; n < tx->vout.size(); ++n) {
                if (nChangePosRet != -1 && int(n) == nChangePosRet) {
                    // Skip the change output to only return the requested coins
                    continue;
                }
                tallyItem.vecInputCoins.emplace_back(tx, n);
                tallyItem.nAmount += tx->vout[n].nValue;
            }
        }
        assert(tallyItem.vecInputCoins.size() == vecAmounts.size());
        reserveDest.KeepDestination();
        return tallyItem;
    }
};

namespace {
// The COINJOIN log lines while it lives: it switches the category on, and its destructor removes the callback before
// its members go and puts the category back, also when the test leaves through an exception.
class CoinJoinLogLines
{
public:
    CoinJoinLogLines()
    {
        // The callback first: if registering it throws, the category is still as it was.
        m_connection = LogInstance().PushBackCallback([this](const std::string& line) {
            const std::lock_guard<std::mutex> lock{m_mutex};
            m_lines.push_back(line);
        });
        LogInstance().EnableCategory(BCLog::COINJOIN);
    }
    ~CoinJoinLogLines()
    {
        LogInstance().DeleteCallback(m_connection);
        if (!m_was_on) LogInstance().DisableCategory(BCLog::COINJOIN);
    }
    CoinJoinLogLines(const CoinJoinLogLines&) = delete;
    CoinJoinLogLines& operator=(const CoinJoinLogLines&) = delete;

    // Whether a line with the text was logged since the last call.
    bool Logged(const std::string& text)
    {
        const std::lock_guard<std::mutex> lock{m_mutex};
        const bool found{std::any_of(m_lines.begin(), m_lines.end(), [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        })};
        m_lines.clear();
        return found;
    }

private:
    const bool m_was_on{LogInstance().WillLogCategory(BCLog::COINJOIN)};
    std::mutex m_mutex;
    std::vector<std::string> m_lines;
    std::list<std::function<void(const std::string&)>>::iterator m_connection;
};
} // namespace

// CJ4-A: the wallet turns a DFCN-scale bit (5-7) away before it lists any coin, as v23.0.0 turns away a bit it does
// not know. CWallet is final, so the listing is observed through its COINJOIN log line.
BOOST_FIXTURE_TEST_CASE(coinjoin_dfcn_scale_bit_returns_before_listing_coins, CTransactionBuilderTestSetup)
{
    CoinJoinLogLines log;
    const std::string listing{"CWallet::SelectTxDSInsByDenomination -- vCoins.size():"};

    std::vector<CTxDSIn> selected;
    // An old bit lists the wallet's ready-to-mix coins and logs how many there are.
    wallet->SelectTxDSInsByDenomination(1 << 0, MAX_MONEY, selected);
    BOOST_CHECK(log.Logged(listing));
    for (const int bit : {1 << 5, 1 << 6, 1 << 7}) {
        BOOST_CHECK(!wallet->SelectTxDSInsByDenomination(bit, MAX_MONEY, selected));
        BOOST_CHECK(selected.empty());
        BOOST_CHECK_MESSAGE(!log.Logged(listing), "a DFCN-scale bit returns before the wallet lists its coins");
    }
}

BOOST_FIXTURE_TEST_CASE(coinjoin_manager_start_stop_tests, CTransactionBuilderTestSetup)
{
    BOOST_CHECK(m_node.cj_ctx->walletman->DoForClient("", [](auto& manager) {
        BOOST_CHECK_EQUAL(manager.IsMixing(), false);
        BOOST_CHECK_EQUAL(manager.StartMixing(), true);
        BOOST_CHECK_EQUAL(manager.IsMixing(), true);
        BOOST_CHECK_EQUAL(manager.StartMixing(), false);
        manager.StopMixing();
        BOOST_CHECK_EQUAL(manager.IsMixing(), false);
    }));
}

static bool WithCoinJoinForTest(interfaces::CoinJoin::Loader& loader, const std::string& name,
                               const std::function<void(interfaces::CoinJoin::Client&)>& func)
{
    return loader.WithClient(name, func);
}

BOOST_FIXTURE_TEST_CASE(coinjoin_callback_excludes_wallet_removal, CTransactionBuilderTestSetup)
{
    auto& loader = *m_node.coinjoin_loader;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> removing;
    auto removing_future = removing.get_future();
    std::promise<void> removed;
    auto removed_future = removed.get_future();
    std::thread remover([&] {
        entered_future.wait();
        removing.set_value();
        try {
            if (!RemoveWallet(wallet, std::nullopt)) throw std::runtime_error("wallet removal failed");
            removed.set_value();
        } catch (...) {
            removed.set_exception(std::current_exception());
        }
    });
    bool removed_during_callback{false};
    const bool visited = WithCoinJoinForTest(loader, "", [&](auto& client) {
        entered.set_value();
        removing_future.wait();
        removed_during_callback = removed_future.wait_for(std::chrono::milliseconds{250}) == std::future_status::ready;
        // Avoid touching the legacy dangling reference in the failing control.
        if (!removed_during_callback) client.isMixing();
    });
    if (!visited) entered.set_value();
    remover.join();
    BOOST_CHECK(visited);
    BOOST_CHECK_MESSAGE(!removed_during_callback, "wallet removal completed while the CoinJoin client was in use");
    BOOST_CHECK_NO_THROW(removed_future.get());
    bool called{false};
    BOOST_CHECK(!WithCoinJoinForTest(loader, "", [&](auto&) { called = true; }));
    BOOST_CHECK(!called);
}

BOOST_FIXTURE_TEST_CASE(coinjoin_callback_missing_and_exception, CTransactionBuilderTestSetup)
{
    auto& loader = *m_node.coinjoin_loader;
    bool called{false};
    BOOST_CHECK(!WithCoinJoinForTest(loader, "missing", [&](auto&) { called = true; }));
    BOOST_CHECK(!called);
    BOOST_CHECK_THROW(WithCoinJoinForTest(loader, "", [](auto&) { throw std::runtime_error("callback error"); }), std::runtime_error);
    BOOST_CHECK(WithCoinJoinForTest(loader, "", [&](auto&) { called = true; }));
    BOOST_CHECK(called);
}

BOOST_FIXTURE_TEST_CASE(coinjoin_newkeypool_lock_order, CTransactionBuilderTestSetup)
{
    // Establish the same map -> wallet order used by mixing maintenance before
    // NewKeyPool invokes its callback while holding cs_wallet (Dash #7259).
    m_node.cj_ctx->walletman->ForEachCJClientMan([&](auto& client) {
        BOOST_REQUIRE(client->StartMixing());
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(!wallet->IsLocked());
    });
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->GetLegacyScriptPubKeyMan()->NewKeyPool());
    }
    m_node.cj_ctx->walletman->ForEachCJClientMan([](auto& client) {
        BOOST_CHECK(!client->IsMixing());
    });
}

BOOST_FIXTURE_TEST_CASE(coinjoin_locked_wallet_cannot_start, CTransactionBuilderTestSetup)
{
    auto locked_wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                   "coinjoin-locked", CreateDummyWalletDatabase());
    locked_wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    {
        LOCK(locked_wallet->cs_wallet);
        locked_wallet->SetupDescriptorScriptPubKeyMans();
    }
    BOOST_REQUIRE(locked_wallet->EncryptWallet(SecureString{"coinjoin-test-passphrase"}));
    BOOST_REQUIRE(locked_wallet->Lock());
    BOOST_REQUIRE(locked_wallet->IsLocked(/*fForMixing=*/true));
    BOOST_REQUIRE(AddWallet(locked_wallet));
    BOOST_CHECK(WithCoinJoinForTest(*m_node.coinjoin_loader, locked_wallet->GetName(), [](auto& client) {
        BOOST_CHECK(!client.startMixing());
        BOOST_CHECK(!client.isMixing());
    }));
    BOOST_CHECK(RemoveWallet(locked_wallet, std::nullopt));
}

BOOST_FIXTURE_TEST_CASE(coinjoin_wallet_add_preserves_disabled_option, CTransactionBuilderTestSetup)
{
    const bool enabled_before = CCoinJoinClientOptions::IsEnabled();
    CCoinJoinClientOptions::SetEnabled(false);
    m_node.coinjoin_loader->AddWallet(wallet);
    BOOST_CHECK(!CCoinJoinClientOptions::IsEnabled());
    CCoinJoinClientOptions::SetEnabled(enabled_before);
}

BOOST_FIXTURE_TEST_CASE(coinjoin_loader_applies_enable_option, CTransactionBuilderTestSetup)
{
    const bool enabled_before = CCoinJoinClientOptions::IsEnabled();
    const bool had_arg = gArgs.IsArgSet("-enablecoinjoin");
    const std::string old_arg = gArgs.GetArg("-enablecoinjoin", "");
    CCoinJoinClientOptions::SetEnabled(false);
    gArgs.ForceSetArg("-enablecoinjoin", "1");
    auto enabled_loader = interfaces::MakeCoinJoinLoader(*m_node.cj_ctx->walletman);
    BOOST_CHECK(CCoinJoinClientOptions::IsEnabled());
    gArgs.ForceSetArg("-enablecoinjoin", "0");
    auto disabled_loader = interfaces::MakeCoinJoinLoader(*m_node.cj_ctx->walletman);
    BOOST_CHECK(!CCoinJoinClientOptions::IsEnabled());
    if (had_arg) gArgs.ForceSetArg("-enablecoinjoin", old_arg);
    else gArgs.ForceRemoveArg("enablecoinjoin");
    CCoinJoinClientOptions::SetEnabled(enabled_before);
    BOOST_CHECK_EQUAL(gArgs.IsArgSet("-enablecoinjoin"), had_arg);
    BOOST_CHECK_EQUAL(gArgs.GetArg("-enablecoinjoin", ""), old_arg);
}

BOOST_FIXTURE_TEST_CASE(coinjoin_autostart_only_initializes_added_wallet, CTransactionBuilderTestSetup)
{
    const bool had_arg = gArgs.IsArgSet("-coinjoinautostart");
    const std::string old_arg = gArgs.GetArg("-coinjoinautostart", "");
    const bool enabled_before = CCoinJoinClientOptions::IsEnabled();
    CCoinJoinClientOptions::SetEnabled(true);
    gArgs.ForceSetArg("-coinjoinautostart", "1");
    auto added_wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                  "coinjoin-added", CreateDummyWalletDatabase());
    BOOST_REQUIRE(AddWallet(added_wallet));
    BOOST_CHECK(WithCoinJoinForTest(*m_node.coinjoin_loader, "", [](auto& client) {
        BOOST_CHECK(!client.isMixing());
    }));
    BOOST_CHECK(WithCoinJoinForTest(*m_node.coinjoin_loader, added_wallet->GetName(), [](auto& client) {
        BOOST_CHECK(client.isMixing());
    }));
    BOOST_CHECK(RemoveWallet(added_wallet, std::nullopt));
    BOOST_CHECK(WithCoinJoinForTest(*m_node.coinjoin_loader, "", [](auto& client) {
        BOOST_CHECK(!client.isMixing());
    }));
    if (had_arg) gArgs.ForceSetArg("-coinjoinautostart", old_arg);
    else gArgs.ForceRemoveArg("coinjoinautostart");
    CCoinJoinClientOptions::SetEnabled(enabled_before);
    BOOST_CHECK_EQUAL(gArgs.IsArgSet("-coinjoinautostart"), had_arg);
    BOOST_CHECK_EQUAL(gArgs.GetArg("-coinjoinautostart", ""), old_arg);
}

// CJ4-B1: both selectors take only a coin a mixing entry can use (P2PKH, the wallet's private key, present in the node's
// chain and mempool now, not spent in its mempool, and there confirmed or InstantSend-locked now), from a wallet that is
// not fully locked, before the coin takes any of the budget, and count the budget and the scripts per round count (the
// ready step builds its entry from one round count). Every other coin here is ready to mix by AvailableCoins, so it is
// the selectors' own rule that leaves it out. The fixture's wallet is not subscribed to the chain's notifications, so
// what the node does after a coin was recorded (a block disconnected, a transaction evicted or added) reaches the wallet
// late, as notifications can. One coin per transaction: an entry takes one output of a transaction.
BOOST_FIXTURE_TEST_CASE(coinjoin_selectors_take_only_mixing_candidates, CTransactionBuilderTestSetup)
{
    const int bit{1 << 0};
    const CAmount amount{CoinJoin::DenominationToAmount(bit)};          // 10.0001
    const CAmount small_amount{CoinJoin::DenominationToAmount(1 << 1)}; // 1.00001
    const auto new_key = [&](bool in_wallet) {
        CKey key;
        key.MakeNewKey(true);
        if (in_wallet) {
            LOCK(wallet->cs_wallet);
            BOOST_REQUIRE(wallet->GetLegacyScriptPubKeyMan()->AddKeyPubKey(key, key.GetPubKey()));
        }
        return key;
    };
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };
    const CKey key_a{new_key(true)}, key_b{new_key(true)}, key_c{new_key(true)}, key_s1{new_key(true)},
        key_s2{new_key(true)}, key_late{new_key(true)}, key_c1{new_key(true)}, key_c2{new_key(true)},
        key_small{new_key(true)}, key_watched{new_key(false)}, key_d{new_key(false)}, key_l{new_key(false)};
    {
        LOCK(wallet->cs_wallet);
        auto* spkm{wallet->GetLegacyScriptPubKeyMan()};
        LOCK(spkm->cs_KeyStore);
        BOOST_REQUIRE(spkm->AddWatchOnly(p2pkh(key_watched), /*nCreateTime=*/1));
    }

    // Confirmed first (a later payment must not spend an unconfirmed change). Round 0: A twice, B; round 1 (a zero-fee,
    // all-denominated spend of a round-0 coin): A, B. Not candidates: pay to pubkey, watch-only. For the wallets of their
    // own below: D (public keys only) and L (lockable). Last, in the tip block: LATE.
    const COutPoint a1{PayOutputs({{p2pkh(key_a), amount}}, true)->GetHash(), 0};
    const COutPoint a2{PayOutputs({{p2pkh(key_a), amount}}, true)->GetHash(), 0};
    const COutPoint b{PayOutputs({{p2pkh(key_b), amount}}, true)->GetHash(), 0};
    const COutPoint s1{PayOutputs({{p2pkh(key_s1), amount}}, true)->GetHash(), 0};
    const COutPoint s2{PayOutputs({{p2pkh(key_s2), amount}}, true)->GetHash(), 0};
    const COutPoint a_r1{MixOnce(s1, amount, p2pkh(key_a))};
    const COutPoint b_r1{MixOnce(s2, amount, p2pkh(key_b))};
    const COutPoint pay_to_pubkey{PayOutputs({{GetScriptForRawPubKey(key_c.GetPubKey()), amount}}, true)->GetHash(), 0};
    const COutPoint watched{PayOutputs({{p2pkh(key_watched), amount}}, true)->GetHash(), 0};
    const COutPoint d{PayOutputs({{p2pkh(key_d), amount}}, true)->GetHash(), 0};
    const COutPoint l{PayOutputs({{p2pkh(key_l), amount}}, true)->GetHash(), 0};
    const COutPoint late{PayOutputs({{p2pkh(key_late), amount}}, true)->GetHash(), 0};
    // In the mempool, own, no InstantSend lock yet: C1 and C2 of 10.0001, SMALL of 1.00001.
    const CTransactionRef c1_tx{PayOutputs({{p2pkh(key_c1), amount}}, false)};
    const CTransactionRef c2_tx{PayOutputs({{p2pkh(key_c2), amount}}, false)};
    const CTransactionRef small_tx{PayOutputs({{p2pkh(key_small), small_amount}}, false)};
    const COutPoint c1{c1_tx->GetHash(), 0}, c2{c2_tx->GetHash(), 0}, small{small_tx->GetHash(), 0};
    const std::set<COutPoint> round0{a1, a2, b, late}, round1{a_r1, b_r1};
    {
        const auto ready{ReadyToMix(*wallet)};
        for (const auto& [outpoint, name] : std::vector<std::pair<COutPoint, std::string>>{
                 {a1, "A, round 0"}, {a2, "A again, round 0"}, {b, "B, round 0"}, {late, "LATE, round 0"},
                 {a_r1, "A, round 1"}, {b_r1, "B, round 1"}, {pay_to_pubkey, "pay to pubkey"}, {watched, "watch-only"},
                 {c1, "C1 in the mempool"}, {c2, "C2 in the mempool"}, {small, "SMALL in the mempool"}}) {
            BOOST_REQUIRE_MESSAGE(ready.count(outpoint), "ready to mix by AvailableCoins: " + name);
        }
        BOOST_REQUIRE_EQUAL(wallet->GetRealOutpointCoinJoinRounds(a1), 0);
        BOOST_REQUIRE_EQUAL(wallet->GetRealOutpointCoinJoinRounds(a_r1), 1);
        BOOST_REQUIRE_EQUAL(wallet->GetRealOutpointCoinJoinRounds(b_r1), 1);
    }

    // The entry selector's choice, with the return value checked against the outcome and an entry's scripts (one round
    // count each) told apart.
    const auto inputs = [&](CWallet& w, CAmount budget) {
        std::vector<CTxDSIn> selected;
        const bool found{w.SelectTxDSInsByDenomination(bit, budget, selected)};
        BOOST_CHECK_EQUAL(found, !selected.empty());
        std::vector<COutPoint> outpoints;
        std::map<int, std::set<CScript>> scripts;
        for (const auto& in : selected) {
            outpoints.push_back(in.prevout);
            BOOST_CHECK_MESSAGE(scripts[in.nRounds].insert(in.prevPubKey).second, "an entry names each script once");
        }
        return outpoints;
    };
    const auto count = [](const std::vector<COutPoint>& v, const COutPoint& o) { return std::count(v.begin(), v.end(), o); };
    const auto count_of = [](const std::vector<COutPoint>& v, const std::set<COutPoint>& set) {
        return std::count_if(v.begin(), v.end(), [&](const COutPoint& o) { return set.count(o) > 0; });
    };
    const auto denominated_amounts = [](CWallet& w, CAmount budget) {
        std::set<CAmount> amounts;
        const bool enough{w.SelectDenominatedAmounts(budget, amounts)};
        return std::make_pair(enough, amounts);
    };

    // Any budget: only candidates; round 0 gives B, LATE and one of A's two coins, round 1 keeps its own A and B.
    for (int i = 0; i < 20; ++i) {
        const auto chosen{inputs(*wallet, MAX_MONEY)};
        BOOST_CHECK_MESSAGE(chosen.size() == 5, "every round count keeps its own scripts");
        for (const auto& outpoint : chosen) {
            BOOST_CHECK_MESSAGE(round0.count(outpoint) || round1.count(outpoint), "the entry selector takes only mixing candidates");
        }
        BOOST_CHECK_EQUAL(count(chosen, b) + count(chosen, late) + count(chosen, a_r1) + count(chosen, b_r1), 4);
        BOOST_CHECK_EQUAL(count(chosen, a1) + count(chosen, a2), 1);
    }
    // A budget of one coin, per round count: exactly one candidate of round 0 and one of round 1.
    for (int i = 0; i < 20; ++i) {
        const auto chosen{inputs(*wallet, amount)};
        BOOST_CHECK_MESSAGE(chosen.size() == 2 && count_of(chosen, round0) == 1 && count_of(chosen, round1) == 1,
                            "the budget is counted per round count");
    }
    BOOST_CHECK(denominated_amounts(*wallet, MAX_MONEY) == std::make_pair(true, std::set<CAmount>{amount}));

    // InstantSend on, and C1, C2 and SMALL locked: the mempool coins are candidates.
    CKey spork_key;
    spork_key.MakeNewKey(false);
    auto& sporkman{*m_node.sporkman};
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash(spork_key.GetPubKey()))));
    BOOST_REQUIRE(sporkman.SetMinSporkKeys(1));
    BOOST_REQUIRE(sporkman.SetPrivKey(EncodeSecret(spork_key)));
    BOOST_REQUIRE(sporkman.UpdateSpork(*Assert(m_node.peerman.get()), SPORK_2_INSTANTSEND_ENABLED, 0));
    for (const CTransactionRef& tx : {c1_tx, c2_tx, small_tx}) {
        llmq::CInstantSendLock islock;
        islock.txid = tx->GetHash();
        for (const auto& in : tx->vin) islock.inputs.push_back(in.prevout);
        islock.cycleHash = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        llmq::StoreInstantSendLockForTest(*m_node.llmq_ctx->isman, islock);
        BOOST_REQUIRE(m_node.llmq_ctx->isman->IsLocked(islock.txid));
    }
    {
        const auto chosen{inputs(*wallet, MAX_MONEY)};
        BOOST_CHECK_EQUAL(count(chosen, c1), 1);
        BOOST_CHECK_EQUAL(count(chosen, c2), 1);
    }
    BOOST_CHECK(denominated_amounts(*wallet, MAX_MONEY) == std::make_pair(true, std::set<CAmount>{amount, small_amount}));
    // A budget of one 10.0001: the larger amount comes first, and then SMALL no longer fits.
    BOOST_CHECK_MESSAGE(denominated_amounts(*wallet, amount) == std::make_pair(true, std::set<CAmount>{amount}),
                        "the amounts a new queue may be started for stay within the budget");

    // C2 evicted from the node's mempool, the wallet not told yet (it still has C2 in the mempool, locked): the node no
    // longer has the coin, so it is no candidate; C1 still is.
    {
        LOCK2(cs_main, m_node.mempool->cs);
        m_node.mempool->removeRecursive(*c2_tx, MemPoolRemovalReason::SIZELIMIT);
    }
    BOOST_REQUIRE(!m_node.mempool->exists(c2_tx->GetHash()));
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetWalletTx(c2_tx->GetHash())->InMempool()));
    BOOST_REQUIRE(m_node.llmq_ctx->isman->IsLocked(c2_tx->GetHash()));
    BOOST_REQUIRE(ReadyToMix(*wallet).count(c2));
    {
        const auto chosen{inputs(*wallet, MAX_MONEY)};
        BOOST_CHECK_MESSAGE(count(chosen, c2) == 0, "a coin the node no longer has is not a candidate");
        BOOST_CHECK_EQUAL(count(chosen, c1), 1);
    }

    // The 10.0001 candidates locked by the user: the 10.0001 coins left (pay to pubkey, watch-only, C2) take none of a
    // 10.0001 budget, which goes to the locked SMALL in the mempool.
    {
        LOCK(wallet->cs_wallet);
        for (const auto& outpoint : {a1, a2, b, late, a_r1, b_r1, c1}) BOOST_REQUIRE(wallet->LockCoin(outpoint));
    }
    BOOST_CHECK_MESSAGE(denominated_amounts(*wallet, amount) == std::make_pair(true, std::set<CAmount>{small_amount}),
                        "coins no entry can use take none of the budget");

    // InstantSend off: C1 (unlocked by the user again) is in the node's mempool, its lock ended, while the wallet's
    // cached answer still says locked.
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->UnlockCoin(c1));
    }
    SetMockTime(GetTime() + 60);
    BOOST_REQUIRE(sporkman.UpdateSpork(*Assert(m_node.peerman.get()), SPORK_2_INSTANTSEND_ENABLED, 4070908800ULL));
    BOOST_REQUIRE(!m_node.llmq_ctx->isman->IsLocked(c1_tx->GetHash()));
    BOOST_REQUIRE(m_node.mempool->exists(c1_tx->GetHash()));
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetWalletTx(c1_tx->GetHash())->IsLockedByInstantSend()));
    BOOST_REQUIRE(ReadyToMix(*wallet).count(c1));
    BOOST_CHECK_MESSAGE(count(inputs(*wallet, MAX_MONEY), c1) == 0,
                        "a mempool coin whose InstantSend lock ended is not a candidate");

    // Nothing usable left: neither selector finds anything, so no queue is joined or started. Unlocking B brings
    // exactly B back.
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->LockCoin(c1));
    }
    BOOST_CHECK(inputs(*wallet, MAX_MONEY).empty());
    BOOST_CHECK_MESSAGE(!denominated_amounts(*wallet, MAX_MONEY).first, "a new queue is not started for coins no entry can use");
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->UnlockCoin(b));
    }
    BOOST_CHECK(inputs(*wallet, MAX_MONEY) == std::vector<COutPoint>{b});
    BOOST_CHECK(denominated_amounts(*wallet, MAX_MONEY) == std::make_pair(true, std::set<CAmount>{amount}));

    // B's spend in the node's mempool, the wallet not told yet (it still lists B): a final transaction with B would
    // conflict with it, so B is no candidate for either selector. With the spend evicted, B is back.
    CMutableTransaction spend_b;
    spend_b.vin.emplace_back(b);
    spend_b.vout.emplace_back(amount - 10000, p2pkh(key_c));
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->SignTransaction(spend_b));
    }
    const CTransactionRef spend_b_tx{MakeTransactionRef(spend_b)};
    BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ProcessTransaction(spend_b_tx).m_result_type) ==
                  MempoolAcceptResult::ResultType::VALID);
    BOOST_REQUIRE(ReadyToMix(*wallet).count(b));
    BOOST_CHECK_MESSAGE(inputs(*wallet, MAX_MONEY).empty(), "a coin spent in the node's mempool is not a candidate");
    BOOST_CHECK_MESSAGE(!denominated_amounts(*wallet, MAX_MONEY).first, "a coin spent in the node's mempool starts no queue");
    {
        LOCK2(cs_main, m_node.mempool->cs);
        m_node.mempool->removeRecursive(*spend_b_tx, MemPoolRemovalReason::SIZELIMIT);
    }
    BOOST_REQUIRE(!m_node.mempool->exists(spend_b_tx->GetHash()));
    BOOST_CHECK(inputs(*wallet, MAX_MONEY) == std::vector<COutPoint>{b});

    // A descriptor wallet without private keys: its public-key descriptor's coin D counts as spendable, yet cannot be
    // signed, so it is no candidate.
    auto public_only = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "cj4b1-public-only",
                                                 CreateMockWalletDatabase());
    public_only->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    public_only->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    {
        FlatSigningProvider provider;
        std::string error;
        std::unique_ptr<Descriptor> desc{Parse("pkh(" + HexStr(key_d.GetPubKey()) + ")", provider, error)};
        BOOST_REQUIRE_MESSAGE(desc, error);
        WalletDescriptor w_desc(std::move(desc), /*creation_time=*/0, /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);
        LOCK(public_only->cs_wallet);
        BOOST_REQUIRE(public_only->AddWalletDescriptor(w_desc, provider, "", /*internal=*/false));
    }
    Rescan(*public_only);
    const auto public_ready{ReadyToMix(*public_only)};
    BOOST_REQUIRE(public_ready.count(d) && public_ready.at(d));
    BOOST_CHECK_MESSAGE(inputs(*public_only, MAX_MONEY).empty(), "a wallet without private keys offers no mixing input");
    BOOST_CHECK(!denominated_amounts(*public_only, MAX_MONEY).first);

    // A fully locked wallet offers nothing, also while its coin L is listed; unlocked for mixing only, it offers L. A
    // wallet of its own, on a database that encryption can rewrite (the fixture's mock database has no path).
    auto lockable = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "cj4b1-lockable",
                                              CreateDummyWalletDatabase());
    {
        LOCK(lockable->cs_wallet);
        lockable->SetupLegacyScriptPubKeyMan();
        BOOST_REQUIRE(lockable->GetLegacyScriptPubKeyMan()->AddKeyPubKey(key_l, key_l.GetPubKey()));
    }
    Rescan(*lockable);
    BOOST_REQUIRE(inputs(*lockable, MAX_MONEY) == std::vector<COutPoint>{l});
    BOOST_REQUIRE(lockable->EncryptWallet(SecureString{"cj4b1-passphrase"}));
    BOOST_REQUIRE(lockable->Lock());
    BOOST_REQUIRE(lockable->IsLocked(/*fForMixing=*/true));
    BOOST_REQUIRE(ReadyToMix(*lockable).count(l));
    BOOST_CHECK_MESSAGE(inputs(*lockable, MAX_MONEY).empty(), "a fully locked wallet offers no mixing input");
    BOOST_CHECK_MESSAGE(!denominated_amounts(*lockable, MAX_MONEY).first, "a fully locked wallet starts no queue");
    BOOST_REQUIRE(lockable->Unlock(SecureString{"cj4b1-passphrase"}, /*fForMixingOnly=*/true));
    BOOST_REQUIRE(!lockable->IsLocked(/*fForMixing=*/true));
    BOOST_CHECK(inputs(*lockable, MAX_MONEY) == std::vector<COutPoint>{l});
    BOOST_CHECK(denominated_amounts(*lockable, MAX_MONEY).first);

    // LATE's block disconnected, the wallet not told yet (it still counts LATE confirmed): the node holds LATE only in
    // its mempool, without a lock, so it is no candidate.
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->UnlockCoin(late));
    }
    BOOST_REQUIRE(count(inputs(*wallet, MAX_MONEY), late) == 1);
    {
        BlockValidationState state;
        CBlockIndex* tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip())};
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, tip));
    }
    BOOST_REQUIRE(m_node.mempool->exists(late.hash));
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetWalletTx(late.hash)->GetDepthInMainChain()) > 0);
    BOOST_REQUIRE(ReadyToMix(*wallet).count(late));
    BOOST_CHECK_MESSAGE(inputs(*wallet, MAX_MONEY) == std::vector<COutPoint>{b},
                        "a coin the node holds unconfirmed and unlocked is not a candidate");
}

// CJ4-B1b: the ready step takes no more than the budget its session joined or started the queue with (at most what one
// session of its bit mixes, GetMaxPoolAmount(nDenom)), refuses a session without one, and an entry names a script once
// also when a range of round counts meets one script twice. A session of the fixture's wallet without a masternode:
// SendDenominate stops at the empty collateral after PrepareDenominate has locked the entry's inputs, so the wallet's
// locked coins show what the ready step took.
BOOST_FIXTURE_TEST_CASE(coinjoin_ready_step_keeps_session_budget, CTransactionBuilderTestSetup)
{
    // The process-wide CoinJoin switch is put back however the case ends.
    struct RestoreEnabled {
        const bool before{CCoinJoinClientOptions::IsEnabled()};
        ~RestoreEnabled() { CCoinJoinClientOptions::SetEnabled(before); }
    } restore_enabled;
    CCoinJoinClientOptions::SetEnabled(true);
    const int bit{1 << 0};
    const CAmount amount{CoinJoin::DenominationToAmount(bit)}; // 10.0001

    // One session's most: v23.0.0's 90.0009 DFCN for the five old bits, nine coins of the amount for a DFCN-scale bit.
    for (int b = 0; b < 5; ++b) BOOST_CHECK_EQUAL(CoinJoin::GetMaxPoolAmount(1 << b), CoinJoin::GetMaxPoolAmount());
    for (int b = 5; b < 8; ++b) {
        BOOST_CHECK_EQUAL(CoinJoin::GetMaxPoolAmount(1 << b), 9 * CoinJoin::DenominationToAmount(1 << b));
    }

    // Twelve confirmed round-0 coins of 10.0001, each to a script of its own.
    std::map<COutPoint, CScript> coins;
    for (int i = 0; i < 12; ++i) {
        CKey key;
        key.MakeNewKey(true);
        {
            LOCK(wallet->cs_wallet);
            BOOST_REQUIRE(wallet->GetLegacyScriptPubKeyMan()->AddKeyPubKey(key, key.GetPubKey()));
        }
        const CScript script{GetScriptForDestination(PKHash(key.GetPubKey()))};
        coins.emplace(COutPoint{PayOutputs({{script, amount}}, true)->GetHash(), 0}, script);
    }
    const auto locked = [&]() { return WITH_LOCK(wallet->cs_wallet, return wallet->ListLockedCoins()); };
    BOOST_REQUIRE(locked().empty());

    BOOST_REQUIRE(m_node.cj_ctx->walletman->DoForClient(wallet->GetName(), [&](CCoinJoinClientManager& clientman) {
        CCoinJoinClientSession session(wallet, *m_node.cj_ctx->walletman, clientman, *m_node.dmnman, *m_node.mn_metaman,
                                       *m_node.mn_sync, *m_node.llmq_ctx->isman, m_node.cj_ctx->queueman,
                                       /*is_masternode=*/false);
        session.nSessionDenom = bit;

        // The ready step's selection, before its random skips: exactly three of the twelve coins for a budget of
        // three, each a fixture coin; v23.0.0's fixed 90.0009 DFCN selects nine.
        std::vector<CTxDSIn> selected;
        BOOST_CHECK(SelectDenominateForTest(session, 3 * amount, selected));
        BOOST_CHECK_MESSAGE(selected.size() == 3, "the ready step selects within the session's budget");
        for (const auto& in : selected) BOOST_CHECK(coins.count(in.prevout));
        BOOST_CHECK(locked().empty());

        // The ready step with a budget of three coins (each run: the inputs it locked, then unlocked again): never
        // more than three, each a candidate of this denomination, each script once. v23.0.0's fixed 90.0009 DFCN
        // would take up to nine of the twelve.
        for (int i = 0; i < 5; ++i) {
            BOOST_CHECK(!SubmitDenominateForTest(session, 3 * amount, *m_node.connman)); // stops at the empty collateral
            const auto taken{locked()};
            std::set<CScript> scripts;
            for (const auto& outpoint : taken) {
                BOOST_CHECK(coins.count(outpoint));
                if (coins.count(outpoint)) scripts.insert(coins.at(outpoint));
            }
            BOOST_CHECK_MESSAGE(!taken.empty() && taken.size() <= 3, "the ready step keeps the session's budget");
            BOOST_CHECK_EQUAL(scripts.size(), taken.size());
            session.UnlockCoins();
            BOOST_REQUIRE(locked().empty());
        }

        // A session without a budget takes nothing.
        BOOST_CHECK(!SubmitDenominateForTest(session, 0, *m_node.connman));
        BOOST_CHECK_MESSAGE(locked().empty(), "a session without a budget takes no input");

        // One script in two round counts (A's coin of round 0 and A's coin of round 1) with B's of round 0: over the
        // range 0-1 the entry takes A once and B, never A twice.
        const auto it{coins.begin()};
        const COutPoint a0{it->first}, a1{std::next(it)->first}, b0{std::next(it, 2)->first};
        const CScript script_a{it->second}, script_b{std::next(it, 2)->second};
        const std::vector<CTxDSIn> ins{CTxDSIn(CTxIn(a0), script_a, 0), CTxDSIn(CTxIn(a1), script_a, 1),
                                       CTxDSIn(CTxIn(b0), script_b, 0)};
        std::vector<std::pair<CTxDSIn, CTxOut>> pairs;
        BOOST_CHECK(PrepareDenominateForTest(session, 0, 1, ins, pairs));
        std::set<CScript> entry_scripts;
        for (const auto& [in, out] : pairs) entry_scripts.insert(in.prevPubKey);
        BOOST_CHECK_MESSAGE(pairs.size() == 2 && entry_scripts.size() == 2, "an entry names each script once");
        BOOST_CHECK(locked().empty());
    }));
}

// CJ4-B2a: the wallet recognises the DFCN-scale amounts (bits 5-7) from the binary's start. A DFCN-scale output counts
// its rounds on a branch of its own (0 unless its transaction is a real mix of its amount; exact, never cached), the old
// amounts keep v23.0.0's count, the mixing target's balance keeps the five old amounts while two new fields carry the
// DFCN-scale coins, and the client still starts no DFCN-scale queue. Every transaction here is valid and mined; the mixes
// spend own coins and coins of keys outside the wallet.
BOOST_FIXTURE_TEST_CASE(coinjoin_dfcn_scale_rounds_and_balances, CTransactionBuilderTestSetup)
{
    struct RestoreOptions {
        const bool enabled{CCoinJoinClientOptions::IsEnabled()};
        const int rounds{CCoinJoinClientOptions::GetRounds()};
        ~RestoreOptions()
        {
            CCoinJoinClientOptions::SetEnabled(enabled);
            CCoinJoinClientOptions::SetRounds(rounds);
        }
    } restore_options;
    CCoinJoinClientOptions::SetEnabled(true);
    CCoinJoinClientOptions::SetRounds(1); // fully mixed from 1 + GetRandomRounds() rounds on
    const CAmount amount{CoinJoin::DenominationToAmount(1 << 5)};     // 100.001
    const CAmount old_amount{CoinJoin::DenominationToAmount(1 << 0)}; // 10.0001
    BOOST_REQUIRE_EQUAL(10 * old_amount, amount);
    const int acp{SIGHASH_ALL | SIGHASH_ANYONECANPAY};

    const auto new_key = [&](bool mine) {
        CKey key;
        key.MakeNewKey(true);
        if (mine) {
            LOCK(wallet->cs_wallet);
            BOOST_REQUIRE(wallet->GetLegacyScriptPubKeyMan()->AddKeyPubKey(key, key.GetPubKey()));
        }
        return key;
    };
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };
    const auto out_of = [](const CTransactionRef& tx, const CScript& script) {
        for (uint32_t i = 0; i < tx->vout.size(); ++i) {
            if (tx->vout[i].scriptPubKey == script) return COutPoint{tx->GetHash(), i};
        }
        BOOST_FAIL("no output to the script");
        return COutPoint{};
    };
    const auto rounds = [&](const COutPoint& outpoint) { return wallet->GetRealOutpointCoinJoinRounds(outpoint); };
    struct In {
        COutPoint prevout;
        CTransactionRef prev_tx;
        CKey key;
        CScript redeem{}; // for a P2SH coin
    };
    // A confirmed coin of `value` to `key` (P2PKH, P2PK, or P2SH of `redeem`), paid from the wallet's ordinary funds. The
    // own DFCN-scale coins the test still holds for a later spend are locked while it pays, so the payment never takes
    // one of them, whatever its coin type (ONLY_NONDENOMINATED) leaves out.
    const auto coin = [&](const CKey& key, CAmount value, bool p2pk = false, const CScript& redeem = CScript()) {
        const CScript script{!redeem.empty() ? GetScriptForDestination(ScriptHash(redeem)) :
                             p2pk ? GetScriptForRawPubKey(key.GetPubKey()) : p2pkh(key)};
        std::vector<COutPoint> locked_now;
        {
            LOCK(wallet->cs_wallet);
            std::vector<COutput> available;
            wallet->AvailableCoins(available);
            for (const auto& out : available) {
                if (!CoinJoin::IsDfcnScaleDenomination(CoinJoin::AmountToDenomination(out.tx->tx->vout[out.i].nValue))) continue;
                const COutPoint outpoint{out.tx->GetHash(), static_cast<uint32_t>(out.i)};
                if (wallet->LockCoin(outpoint)) locked_now.push_back(outpoint);
            }
        }
        const CTransactionRef tx{PayOutputs({{script, value}}, true)};
        {
            LOCK(wallet->cs_wallet);
            for (const auto& outpoint : locked_now) BOOST_REQUIRE(wallet->UnlockCoin(outpoint));
        }
        return In{out_of(tx, script), tx, key, redeem};
    };
    // A mined, zero-fee spend of `ins` to `outs`, each input signed with its key and `hash_type`; inputs and outputs in
    // BIP69 order, or the outputs in reverse BIP69 order when `bip69` is false. A non-empty `own_prefix` goes before the
    // signature of every own input (valid in a block: the signatures do not cover scriptSigs). make_spend builds and
    // checks it, mine gives it to the wallet and mines it, spend does both.
    const auto make_spend = [&](std::vector<In> ins, std::vector<CTxOut> outs, bool bip69, int hash_type, const CScript& own_prefix = CScript()) {
        std::sort(ins.begin(), ins.end(), [](const In& a, const In& b) { return CompareInputBIP69()(CTxIn(a.prevout), CTxIn(b.prevout)); });
        std::sort(outs.begin(), outs.end(), CompareOutputBIP69());
        if (!bip69) std::reverse(outs.begin(), outs.end());
        CMutableTransaction mtx;
        for (const auto& in : ins) mtx.vin.emplace_back(in.prevout);
        mtx.vout = outs;
        for (size_t i = 0; i < ins.size(); ++i) {
            FillableSigningProvider provider;
            BOOST_REQUIRE(provider.AddKey(ins[i].key));
            if (!ins[i].redeem.empty()) BOOST_REQUIRE(provider.AddCScript(ins[i].redeem));
            BOOST_REQUIRE(SignSignature(provider, *ins[i].prev_tx, mtx, i, hash_type));
        }
        for (size_t i = 0; i < ins.size() && !own_prefix.empty(); ++i) {
            if (WITH_LOCK(wallet->cs_wallet, return wallet->IsMine(ins[i].prev_tx->vout[ins[i].prevout.n]) & ISMINE_SPENDABLE)) {
                mtx.vin[i].scriptSig.insert(mtx.vin[i].scriptSig.begin(), own_prefix.begin(), own_prefix.end());
            }
        }
        {
            // The block that will mine it is valid; if not, the reject reason says why.
            const CBlock block{CreateBlock({mtx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), m_node.chainman->ActiveChainstate())};
            LOCK(cs_main);
            BlockValidationState state;
            BOOST_REQUIRE_MESSAGE(TestBlockValidity(state, *Assert(m_node.llmq_ctx)->clhandler, *Assert(m_node.evodb), Params(),
                                                    m_node.chainman->ActiveChainstate(), block,
                                                    m_node.chainman->ActiveChain().Tip(), /*fCheckPOW=*/true,
                                                    /*fCheckMerkleRoot=*/true),
                                  "the spend's block is valid: " + state.ToString());
        }
        return mtx;
    };
    const auto mine = [&](const CMutableTransaction& mtx) {
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        {
            LOCK2(wallet->cs_wallet, cs_main);
            wallet->CommitTransaction(tx, {}, {});
        }
        Confirm(tx);
        return tx;
    };
    const auto spend = [&](std::vector<In> ins, std::vector<CTxOut> outs, bool bip69, int hash_type, const CScript& own_prefix = CScript()) {
        return mine(make_spend(std::move(ins), std::move(outs), bip69, hash_type, own_prefix));
    };
    // A real mix of one own coin and one foreign coin to a new own and a new foreign address.
    const auto mix_once = [&](const In& own, const CKey& own_to) {
        const In foreign{coin(new_key(false), amount)};
        const CTransactionRef tx{spend({own, foreign}, {CTxOut(amount, p2pkh(own_to)), CTxOut(amount, p2pkh(new_key(false)))}, true, acp)};
        return In{out_of(tx, p2pkh(own_to)), tx, own_to};
    };

    // An ordinary incoming DFCN-scale coin, paid by another party (no own input), reads 0 (v23.0.0 read -2: no
    // denomination) and is recognised.
    const CKey a_key{new_key(true)};
    const CTransactionRef a_tx{spend({coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(a_key))}, true, SIGHASH_ALL)};
    const In a{out_of(a_tx, p2pkh(a_key)), a_tx, a_key};
    BOOST_CHECK_MESSAGE(rounds(a.prevout) == 0, "an ordinary incoming DFCN-scale coin reads 0");
    BOOST_CHECK(wallet->IsDenominated(a.prevout));

    // Real mixes: one round each, four in a row (fully mixed at 1 + GetRandomRounds() rounds).
    In chain{a};
    for (int r = 1; r <= 4; ++r) {
        chain = mix_once(chain, new_key(true));
        BOOST_CHECK_MESSAGE(rounds(chain.prevout) == r, "each real DFCN-scale mix counts one round more");
    }
    BOOST_REQUIRE_GE(4, 1 + CCoinJoinClientOptions::GetRandomRounds());
    BOOST_CHECK(wallet->IsFullyMixed(chain.prevout));

    // Not a real mix, each 0: outputs out of BIP69 order; an output to an own input's script; an own P2PK input; an own
    // P2SH input in the client's scriptSig shape; an own input signed without ANYONECANPAY, also behind an extra push;
    // own debit unequal to own credit (two own inputs, one own output).
    std::vector<COutPoint> unmixed;
    {
        const In own{coin(new_key(true), amount)};
        const CKey to{new_key(true)};
        const auto tx{spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false)))}, false, acp)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to))) == 0, "outputs out of BIP69 order give 0");
        unmixed.push_back(out_of(tx, p2pkh(to)));
    }
    {
        const In own{coin(new_key(true), amount)};
        const auto tx{spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(own.key)), CTxOut(amount, p2pkh(new_key(false)))}, true, acp)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(own.key))) == 0, "an output to an own input's script gives 0");
        unmixed.push_back(out_of(tx, p2pkh(own.key)));
    }
    {
        const In own{coin(new_key(true), amount, /*p2pk=*/true)};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->IsMine(own.prev_tx->vout[own.prevout.n]) & ISMINE_SPENDABLE));
        const CKey to{new_key(true)};
        const auto tx{spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false)))}, true, acp)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to))) == 0, "an own P2PK input gives 0");
        unmixed.push_back(out_of(tx, p2pkh(to)));
    }
    {
        // An own P2SH input in the client's two-push shape (<signature> <redeem script>, the redeem script a P2PK of an
        // own key): the P2PKH check, not the shape, gives 0 here.
        const CKey key{new_key(true)};
        const CScript redeem{GetScriptForRawPubKey(key.GetPubKey())};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetLegacyScriptPubKeyMan()->AddCScript(redeem)));
        const In own{coin(key, amount, /*p2pk=*/false, redeem)};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->IsMine(own.prev_tx->vout[own.prevout.n]) & ISMINE_SPENDABLE));
        const CKey to{new_key(true)};
        const auto tx{spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false)))}, true, acp)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to))) == 0, "an own P2SH input in the client's shape gives 0");
        unmixed.push_back(out_of(tx, p2pkh(to)));
    }
    {
        const In own{coin(new_key(true), amount)};
        const CKey to{new_key(true)};
        const auto tx{spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false)))}, true, SIGHASH_ALL)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to))) == 0, "an own input signed without ANYONECANPAY gives 0");
        unmixed.push_back(out_of(tx, p2pkh(to)));
    }
    {
        // The same, behind a push that ends in the ANYONECANPAY hash type: OP_CHECKSIG still reads the SIGHASH_ALL one.
        const In own{coin(new_key(true), amount)};
        const CKey to{new_key(true)};
        const CScript prefix{CScript() << std::vector<unsigned char>{0x01, SIGHASH_ALL | SIGHASH_ANYONECANPAY}};
        const auto tx{spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false)))}, true, SIGHASH_ALL, prefix)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to))) == 0, "an own SIGHASH_ALL input behind an extra push gives 0");
        unmixed.push_back(out_of(tx, p2pkh(to)));
    }
    {
        const CKey to{new_key(true)};
        const auto tx{spend({coin(new_key(true), amount), coin(new_key(true), amount), coin(new_key(false), amount)},
                            {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false))), CTxOut(amount, p2pkh(new_key(false)))}, true, acp)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to))) == 0, "own debit unequal to own credit gives 0");
        unmixed.push_back(out_of(tx, p2pkh(to)));
    }

    // Old amounts keep v23.0.0's count: a zero-fee, all-old spend of an old coin counts one round (the control); an old
    // coin beside a DFCN-scale sibling reads 0; an old coin whose parent is a DFCN-scale mix output reads 0 (the parent
    // is no denomination to the old count), while the parent still reads its own count.
    const In old_coin{coin(new_key(true), old_amount)};
    const CKey old_to{new_key(true)};
    const COutPoint old_mixed{MixOnce(old_coin.prevout, old_amount, p2pkh(old_to))};
    BOOST_CHECK_EQUAL(rounds(old_mixed), 1);
    {
        const CKey to_old{new_key(true)}, to_new{new_key(true)};
        const auto tx{spend({coin(new_key(true), old_amount), coin(new_key(true), amount)}, {CTxOut(old_amount, p2pkh(to_old)), CTxOut(amount, p2pkh(to_new))}, true, SIGHASH_ALL)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(to_old))) == 0, "an old coin beside a DFCN-scale sibling reads 0");
        unmixed.push_back(out_of(tx, p2pkh(to_new)));
    }
    {
        const In parent{mix_once(coin(new_key(true), amount), new_key(true))};
        BOOST_REQUIRE_EQUAL(rounds(parent.prevout), 1);
        std::vector<CTxOut> outs;
        std::vector<CKey> keys;
        for (int i = 0; i < 10; ++i) {
            keys.push_back(new_key(true));
            outs.emplace_back(old_amount, p2pkh(keys.back()));
        }
        const auto tx{spend({parent}, outs, true, SIGHASH_ALL)};
        BOOST_CHECK_MESSAGE(rounds(out_of(tx, p2pkh(keys.front()))) == 0, "an old coin with a DFCN-scale parent reads 0");
        BOOST_CHECK_MESSAGE(rounds(parent.prevout) == 1, "the DFCN-scale parent keeps its own count");
    }

    // Balances: the target's figure keeps the five old amounts (here only the old mixed coin can be fully mixed: the
    // other old coins have 0 rounds); the fully mixed DFCN-scale coin and all unspent own DFCN-scale coins fill the two
    // DFCN-scale sums a caller asks GetBalance for. A transaction with a DFCN-scale output caches no mixed credit.
    {
        LOCK(wallet->cs_wallet);
        CAmount dfcn_mixed{-1}, dfcn_all{-1};
        const auto bal{wallet->GetBalance(0, true, false, nullptr, &dfcn_mixed, &dfcn_all)};
        const CAmount old_anonymized{wallet->IsFullyMixed(old_mixed) ? old_amount : 0};
        BOOST_CHECK_MESSAGE(bal.m_anonymized == old_anonymized, "the target's mixed balance keeps the five old amounts");
        BOOST_CHECK_EQUAL(wallet->GetBalance().m_anonymized, bal.m_anonymized); // asking for the DFCN-scale sums changes nothing else
        BOOST_CHECK_MESSAGE(dfcn_mixed == amount, "the fully mixed DFCN-scale coin is the DFCN-scale mixed balance");
        BOOST_CHECK_MESSAGE(dfcn_all == amount * CAmount(1 + unmixed.size()), "every unspent own DFCN-scale coin counts");
        BOOST_CHECK_MESSAGE(!wallet->GetWalletTx(chain.prevout.hash)->m_amounts[CWalletTx::ANON_CREDIT].m_cached[ISMINE_SPENDABLE],
                            "a transaction with a DFCN-scale output caches no mixed credit");
        BOOST_CHECK(wallet->GetWalletTx(old_mixed.hash)->m_amounts[CWalletTx::ANON_CREDIT].m_cached[ISMINE_SPENDABLE]);
    }

    // Coin types: a DFCN-scale coin is ready to mix or fully mixed by its rounds; the client still starts no queue of a
    // DFCN-scale bit (the amounts it may start one for are the old ones only).
    const auto ready{ReadyToMix(*wallet)};
    for (const auto& outpoint : unmixed) BOOST_CHECK_MESSAGE(ready.count(outpoint), "an unmixed DFCN-scale coin is ready to mix");
    BOOST_CHECK(!ready.count(chain.prevout));
    {
        std::vector<COutput> mixed;
        CCoinControl coin_control;
        coin_control.nCoinType = CoinType::ONLY_FULLY_MIXED;
        LOCK(wallet->cs_wallet);
        wallet->AvailableCoins(mixed, &coin_control);
        BOOST_CHECK_MESSAGE(std::any_of(mixed.begin(), mixed.end(), [&](const COutput& out) { return COutPoint{out.tx->GetHash(), static_cast<uint32_t>(out.i)} == chain.prevout; }),
                            "a fully mixed DFCN-scale coin is fully mixed for a mixed-only payment");
    }
    {
        // A real mixed-only payment of most of the coin: its input is the fully mixed DFCN-scale coin (no change; the
        // rest is its fee).
        std::vector<CRecipient> recipients{{p2pkh(new_key(false)), amount - 50000, false}};
        CTransactionRef tx;
        CAmount fee;
        int change_pos{-1};
        bilingual_str error;
        CCoinControl coin_control;
        coin_control.UseCoinJoin(true);
        coin_control.m_feerate = CFeeRate(1000);
        FeeCalculation fee_calc;
        const bool created{wallet->CreateTransaction(recipients, tx, fee, change_pos, error, coin_control, fee_calc)};
        BOOST_CHECK_MESSAGE(created && tx->vin.size() == 1 && tx->vin[0].prevout == chain.prevout && tx->vout.size() == 1 && fee == 50000,
                            "a mixed-only payment spends the fully mixed DFCN-scale coin: " + error.original);
    }
    {
        // The old client's funding stays v23.0.0's: a fully mixed DFCN-scale coin is still funds there (B2b leaves every
        // DFCN-scale candidate out), so locking it lowers both anonymizable balances by its amount.
        const auto funding = [&] {
            return std::make_pair(wallet->GetAnonymizableBalance(/*fSkipDenominated=*/false),
                                  wallet->GetAnonymizableBalance(/*fSkipDenominated=*/true));
        };
        const auto with_coin{funding()};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->LockCoin(chain.prevout)));
        const auto without_coin{funding()};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->UnlockCoin(chain.prevout)));
        BOOST_CHECK_MESSAGE(with_coin.first - without_coin.first == amount && with_coin.second - without_coin.second == amount,
                            "the old client still funds from a fully mixed DFCN-scale coin, as v23.0.0");
    }
    {
        std::vector<COutput> nondenominated;
        CCoinControl coin_control;
        coin_control.nCoinType = CoinType::ONLY_NONDENOMINATED;
        LOCK(wallet->cs_wallet);
        wallet->AvailableCoins(nondenominated, &coin_control);
        BOOST_CHECK(!nondenominated.empty()); // the wallet's ordinary funds
        BOOST_CHECK_MESSAGE(std::none_of(nondenominated.begin(), nondenominated.end(), [](const COutput& out) { return CoinJoin::IsDfcnScaleDenomination(CoinJoin::AmountToDenomination(out.tx->tx->vout[out.i].nValue)); }),
                            "coin type 3 leaves the DFCN-scale coins out");
    }
    std::set<CAmount> amounts;
    wallet->SelectDenominatedAmounts(MAX_MONEY, amounts);
    BOOST_CHECK_MESSAGE(!amounts.count(amount), "the client starts no DFCN-scale queue yet");

    // What the GUI reads: a mixed-only payment may spend the fully mixed DFCN-scale coin, and a DFCN-scale mix shows as a
    // mixing transaction.
    {
        const auto gui_wallet{interfaces::MakeWallet(wallet)};
        CCoinControl coin_control;
        coin_control.UseCoinJoin(true);
        const auto bal{WITH_LOCK(wallet->cs_wallet, return wallet->GetBalance(0, coin_control.m_avoid_address_reuse, false, &coin_control))};
        BOOST_CHECK_MESSAGE(gui_wallet->getAvailableBalance(coin_control) == bal.m_anonymized + amount,
                            "a mixed-only payment can spend the fully mixed DFCN-scale coin");
        BOOST_CHECK_MESSAGE(gui_wallet->getWalletTx(chain.prevout.hash).is_denominate, "a DFCN-scale mix shows as a mixing transaction");
    }

    // Generated coins: an own DFCN-scale coinbase output counts only once mature, as the old denominated balance holds
    // generated coins (the salt guards read both).
    {
        const auto dfcn_denominated = [&] {
            CAmount all{-1};
            wallet->GetBalance(0, true, false, nullptr, nullptr, &all);
            return all;
        };
        const auto tell_wallet = [&] {
            LOCK2(wallet->cs_wallet, cs_main);
            wallet->SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        };
        const CAmount before{dfcn_denominated()};
        const CScript miner{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
        CBlock block{CreateBlock({}, miner, m_node.chainman->ActiveChainstate())};
        CMutableTransaction coinbase{*block.vtx[0]};
        const auto miner_out{std::find_if(coinbase.vout.begin(), coinbase.vout.end(), [&](const CTxOut& out) { return out.scriptPubKey == miner; })};
        BOOST_REQUIRE(miner_out != coinbase.vout.end() && miner_out->nValue > amount);
        miner_out->nValue -= amount;
        coinbase.vout.emplace_back(amount, p2pkh(new_key(true)));
        block.vtx[0] = MakeTransactionRef(coinbase);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;
        BOOST_REQUIRE(Assert(m_node.chainman)->ProcessNewBlock(Params(), std::make_shared<const CBlock>(block), true, nullptr));
        const int height{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height())};
        BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash());
        wallet->blockConnected(block, height);
        tell_wallet();
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetWalletTx(block.vtx[0]->GetHash()) != nullptr));
        BOOST_CHECK_MESSAGE(dfcn_denominated() == before, "an immature generated DFCN-scale coin does not count");
        for (int i = 0; i < COINBASE_MATURITY; ++i) CreateAndProcessBlock({}, miner);
        tell_wallet();
        BOOST_CHECK_MESSAGE(dfcn_denominated() == before + amount, "a mature generated DFCN-scale coin counts");
    }

    // A DFCN-scale output asked about before the wallet knows its transaction (the old code keeps -1 for it) reads its
    // own count once the wallet learns the transaction.
    {
        const In own{mix_once(coin(new_key(true), amount), new_key(true))}; // one round
        const CKey to{new_key(true)};
        const CMutableTransaction mtx{make_spend({own, coin(new_key(false), amount)}, {CTxOut(amount, p2pkh(to)), CTxOut(amount, p2pkh(new_key(false)))}, true, acp)};
        const CTransactionRef preview{MakeTransactionRef(mtx)};
        const COutPoint later{out_of(preview, p2pkh(to))};
        BOOST_REQUIRE_EQUAL(rounds(later), -1);
        mine(mtx);
        BOOST_CHECK_MESSAGE(rounds(later) == 2, "a DFCN-scale output asked about before its transaction was known reads its count");
    }

    // The old amounts keep v23.0.0's query order, on which their round cache depends: a zero-fee own spend of 20.0002 to
    // two 10.0001 coins, the first spent on through eighteen zero-fee old spends. Coin type 2, held to the first spend's
    // depth, asks the spent coin's rounds before skipping it, as v23.0.0 does, and caches 0, so the chain's end reads 18;
    // asked first, the end would read 19, the depth guard answering for the first spend's ordinary input.
    {
        const In twenty{coin(new_key(true), 2 * old_amount)};
        const CKey key_a{new_key(true)}, key_b{new_key(true)};
        const CTransactionRef split{spend({twenty}, {CTxOut(old_amount, p2pkh(key_a)), CTxOut(old_amount, p2pkh(key_b))}, true, SIGHASH_ALL)};
        In link{out_of(split, p2pkh(key_a)), split, key_a};
        for (int i = 0; i < 18; ++i) {
            const CKey next{new_key(true)};
            const CTransactionRef tx{spend({link}, {CTxOut(old_amount, p2pkh(next))}, true, SIGHASH_ALL)};
            link = In{out_of(tx, p2pkh(next)), tx, next};
        }
        BOOST_REQUIRE_EQUAL(MAX_COINJOIN_ROUNDS + CCoinJoinClientOptions::GetRandomRounds(), 19);
        {
            std::vector<COutput> ready;
            CCoinControl coin_control;
            coin_control.nCoinType = CoinType::ONLY_READY_TO_MIX;
            LOCK(wallet->cs_wallet);
            coin_control.m_min_depth = coin_control.m_max_depth = wallet->GetWalletTx(split->GetHash())->GetDepthInMainChain();
            wallet->AvailableCoins(ready, &coin_control);
        }
        BOOST_CHECK_MESSAGE(rounds(link.prevout) == 18, "the old amounts' round count follows v23.0.0's query order");
    }

    // Coin types 1 and 2 walk and limit every other coin as v23.0.0 does, and the DFCN-scale coins join after the walk
    // under the same limits. One transaction, alone at its depth, pays two DFCN-scale coins and then two 10.0001 coins,
    // in that fixed output order: a count limit of three, or a sum limit one duff above the two old coins, lists both
    // old coins and the first DFCN-scale coin only (inline, the walk would meet the DFCN-scale coins first; without the
    // limits after the walk, both DFCN-scale coins would join).
    {
        const CKey d1{new_key(true)}, d2{new_key(true)}, o1{new_key(true)}, o2{new_key(true)};
        const In funds{coin(new_key(true), 2 * amount + 2 * old_amount)};
        const CTransactionRef four{spend({funds}, {CTxOut(amount, p2pkh(d1)), CTxOut(amount, p2pkh(d2)), CTxOut(old_amount, p2pkh(o1)), CTxOut(old_amount, p2pkh(o2))},
                                         /*bip69=*/false, SIGHASH_ALL)}; // reverse BIP69: the large amounts first
        BOOST_REQUIRE(four->vout[0].nValue == amount && four->vout[1].nValue == amount && four->vout[2].nValue == old_amount && four->vout[3].nValue == old_amount);
        const auto list = [&](CAmount min_sum, uint64_t max_count) {
            LOCK(wallet->cs_wallet);
            std::vector<COutput> coins;
            CCoinControl coin_control;
            coin_control.nCoinType = CoinType::ONLY_READY_TO_MIX;
            coin_control.m_min_depth = coin_control.m_max_depth = wallet->GetWalletTx(four->GetHash())->GetDepthInMainChain();
            wallet->AvailableCoins(coins, &coin_control, 1, MAX_MONEY, min_sum, max_count);
            std::vector<int> ret;
            for (const auto& out : coins) {
                BOOST_REQUIRE(out.tx->GetHash() == four->GetHash());
                ret.push_back(out.i);
            }
            std::sort(ret.begin(), ret.end());
            return ret;
        };
        BOOST_REQUIRE(list(MAX_MONEY, 0) == std::vector<int>({0, 1, 2, 3}));
        const std::vector<int> expected{0, 2, 3};
        BOOST_CHECK_MESSAGE(list(MAX_MONEY, 3) == expected, "a count limit meets the old coins first and stops at the first DFCN-scale coin");
        BOOST_CHECK_MESSAGE(list(2 * old_amount + 1, 0) == expected, "a sum limit meets the old coins first and stops at the first DFCN-scale coin");
    }

    // The fee ceiling of a mixed-only payment on the multi-coin path (the note's one hundred 10 000.1 coins paying
    // 1 000 000 DFCN, scaled to the fixture's funds): two fully mixed DFCN-scale coins, each smaller than the payment,
    // cover all but 0.101 DFCN of it, which without change would be the fee. Refused at the default ceiling of 0.1 DFCN;
    // with the ceiling raised, the same payment takes two DFCN-scale coins and leaves exactly that fee.
    {
        In second{coin(new_key(true), amount)};
        for (int r = 1; r <= 4; ++r) second = mix_once(second, new_key(true));
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->IsFullyMixed(second.prevout)));
        BOOST_REQUIRE_EQUAL(wallet->m_default_max_tx_fee, COIN / 10);
        const CAmount target{2 * amount - 10100000};
        const auto pay = [&](CTransactionRef& tx, CAmount& fee) {
            std::vector<CRecipient> recipients{{p2pkh(new_key(false)), target, false}};
            int change_pos{-1};
            bilingual_str error;
            CCoinControl coin_control;
            coin_control.UseCoinJoin(true);
            coin_control.m_feerate = CFeeRate(1000);
            FeeCalculation fee_calc;
            return wallet->CreateTransaction(recipients, tx, fee, change_pos, error, coin_control, fee_calc);
        };
        CTransactionRef tx;
        CAmount fee{0};
        BOOST_CHECK_MESSAGE(!pay(tx, fee), "a mixed-only payment above the fee ceiling is refused");
        wallet->m_default_max_tx_fee = COIN;
        const bool paid{pay(tx, fee)};
        wallet->m_default_max_tx_fee = COIN / 10;
        BOOST_REQUIRE_MESSAGE(paid, "the same payment under a raised ceiling");
        BOOST_CHECK_EQUAL(tx->vin.size(), 2U);
        BOOST_CHECK_EQUAL(tx->vout.size(), 1U);
        BOOST_CHECK_EQUAL(fee, 10100000);
        LOCK(wallet->cs_wallet);
        for (const auto& in : tx->vin) {
            BOOST_CHECK(wallet->GetWalletTx(in.prevout.hash)->tx->vout[in.prevout.n].nValue == amount && wallet->IsFullyMixed(in.prevout));
        }
    }

    // With CoinJoin disabled the DFCN-scale sums are zero, as GetBalance's mixing fields are (the salt guards read both).
    CCoinJoinClientOptions::SetEnabled(false);
    {
        CAmount dfcn_mixed{-1}, dfcn_all{-1};
        wallet->GetBalance(0, true, false, nullptr, &dfcn_mixed, &dfcn_all);
        BOOST_CHECK_MESSAGE(dfcn_mixed == 0 && dfcn_all == 0, "with CoinJoin disabled the balance has no DFCN-scale coins");
    }
}

BOOST_FIXTURE_TEST_CASE(CTransactionBuilderTest, CTransactionBuilderTestSetup)
{
    // NOTE: Mock wallet version is FEATURE_BASE which means that it uses uncompressed pubkeys
    // (65 bytes instead of 33 bytes) and we use Low R signatures, so CTxIn size is 179 bytes.
    // Each output is 34 bytes, vin and vout compact sizes are 1 byte each.
    // Therefore base size (i.e. for a tx with 1 input, 0 outputs) is expected to be
    // 4(n32bitVersion) + 1(vin size) + 179(vin[0]) + 1(vout size) + 4(nLockTime) = 189 bytes.

    minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
    // Tests with single outpoint tallyItem
    {
        CompactTallyItem tallyItem = GetTallyItem({4999});
        CTransactionBuilder txBuilder(*wallet, tallyItem);

        BOOST_CHECK_EQUAL(txBuilder.CountOutputs(), 0);
        BOOST_CHECK_EQUAL(txBuilder.GetAmountInitial(), tallyItem.nAmount);
        BOOST_CHECK_EQUAL(txBuilder.GetAmountLeft(), 4810);         // 4999 - 189

        BOOST_CHECK(txBuilder.CouldAddOutput(4776));                // 4810 - 34
        BOOST_CHECK(!txBuilder.CouldAddOutput(4777));

        BOOST_CHECK(txBuilder.CouldAddOutput(0));
        BOOST_CHECK(!txBuilder.CouldAddOutput(-1));

        BOOST_CHECK(txBuilder.CouldAddOutputs({1000, 1000, 2708})); // (4810 - 34 * 3) split in 3 outputs
        BOOST_CHECK(!txBuilder.CouldAddOutputs({1000, 1000, 2709}));

        BOOST_CHECK_EQUAL(txBuilder.AddOutput(4999), nullptr);
        BOOST_CHECK_EQUAL(txBuilder.AddOutput(-1), nullptr);

        CTransactionBuilderOutput* output = txBuilder.AddOutput();
        BOOST_CHECK(output->UpdateAmount(txBuilder.GetAmountLeft()));
        BOOST_CHECK(output->UpdateAmount(1));
        BOOST_CHECK(output->UpdateAmount(output->GetAmount() + txBuilder.GetAmountLeft()));
        BOOST_CHECK(!output->UpdateAmount(output->GetAmount() + 1));
        BOOST_CHECK(!output->UpdateAmount(0));
        BOOST_CHECK(!output->UpdateAmount(-1));
        BOOST_CHECK_EQUAL(txBuilder.CountOutputs(), 1);

        bilingual_str strResult;
        BOOST_CHECK(txBuilder.Commit(strResult));
        CWalletTx& wtx = AddTxToChain(uint256S(strResult.original));
        BOOST_CHECK_EQUAL(wtx.tx->vout.size(), txBuilder.CountOutputs()); // should have no change output
        BOOST_CHECK_EQUAL(wtx.tx->vout[0].nValue, output->GetAmount());
        BOOST_CHECK(wtx.tx->vout[0].scriptPubKey == output->GetScript());
    }
    // Tests with multiple outpoint tallyItem
    {
        CompactTallyItem tallyItem = GetTallyItem({10000, 20000, 30000, 40000, 50000});
        CTransactionBuilder txBuilder(*wallet, tallyItem);
        std::vector<CTransactionBuilderOutput*> vecOutputs;
        bilingual_str strResult;

        auto output = txBuilder.AddOutput(100);
        BOOST_CHECK(output != nullptr);
        BOOST_CHECK(!txBuilder.Commit(strResult));

        if (output != nullptr) {
            output->UpdateAmount(1000);
            vecOutputs.push_back(output);
        }
        while (vecOutputs.size() < 100) {
            output = txBuilder.AddOutput(1000 + vecOutputs.size());
            if (output == nullptr) {
                break;
            }
            vecOutputs.push_back(output);
        }
        BOOST_CHECK_EQUAL(vecOutputs.size(), 100);
        BOOST_CHECK_EQUAL(txBuilder.CountOutputs(), vecOutputs.size());
        BOOST_CHECK(txBuilder.Commit(strResult));
        CWalletTx& wtx = AddTxToChain(uint256S(strResult.original));
        BOOST_CHECK_EQUAL(wtx.tx->vout.size(), txBuilder.CountOutputs() + 1); // should have change output
        for (const auto& out : wtx.tx->vout) {
            auto it = std::find_if(vecOutputs.begin(), vecOutputs.end(), [&](CTransactionBuilderOutput* output) -> bool {
                return output->GetAmount() == out.nValue && output->GetScript() == out.scriptPubKey;
            });
            if (it != vecOutputs.end()) {
                vecOutputs.erase(it);
            } else {
                // change output
                BOOST_CHECK_EQUAL(txBuilder.GetAmountLeft() - 34, out.nValue);
            }
        }
        BOOST_CHECK(vecOutputs.size() == 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
