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
#include <masternode/meta.h>
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
#include <array>
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
// step with a given session budget, one dry PrepareDenominate over a range of round counts, the collateral maker and the
// denomination creator.
bool MakeCollateralAmountsForTest(CCoinJoinClientSession& session) { return session.MakeCollateralAmounts(); }

// CJ4-B2b3: the DFCN-scale attempt's collateral maker and queue start, the automatic attempt below its height read with the
// gate and the coin toss given, and the session's collateral transaction.
bool MakeCollateralAmountsOrdinaryForTest(CCoinJoinClientSession& session) { return session.MakeCollateralAmountsOrdinary(); }

bool StartNewDfcnScaleQueueForTest(CCoinJoinClientSession& session, const std::array<CAmount, 3>& work, CConnman& connman)
{
    return session.StartNewDfcnScaleQueue(work, connman);
}

bool DoAttemptForTest(CCoinJoinClientSession& session, ChainstateManager& chainman, CConnman& connman,
                      const CTxMemPool& mempool, bool fDfcnOpen, const std::function<bool()>& toss)
{
    return session.DoAttempt(chainman, connman, mempool, /*fDryRun=*/false, fDfcnOpen, toss);
}

uint256 CollateralTxHashForTest(const CCoinJoinClientSession& session) { return session.txMyCollateral.GetHash(); }

bool CreateDenominatedForTest(CCoinJoinClientSession& session, CAmount nBalanceToDenominate)
{
    return session.CreateDenominated(nBalanceToDenominate);
}

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

    // Locks every output of the wallet's transactions but `keep`: the old client's groups then hold `keep` only (its
    // tally skips a locked coin as if it were not there, and this wallet learns no coinbase of the blocks mined later).
    void LockAllBut(const std::set<COutPoint>& keep)
    {
        LOCK(wallet->cs_wallet);
        wallet->UnlockAllCoins();
        for (const auto& [hash, wtx] : wallet->mapWallet) {
            for (uint32_t i = 0; i < wtx.tx->vout.size(); ++i) {
                if (!keep.count(COutPoint{hash, i})) wallet->LockCoin(COutPoint{hash, i});
            }
        }
    }

    // A new key of this wallet.
    CKey NewWalletKey()
    {
        CKey key;
        key.MakeNewKey(true);
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->GetLegacyScriptPubKeyMan()->AddKeyPubKey(key, key.GetPubKey()));
        return key;
    }

    // The output of `tx` that pays `amount` to `script`.
    static COutPoint OutputOf(const CTransactionRef& tx, const CScript& script, CAmount amount)
    {
        for (uint32_t i = 0; i < tx->vout.size(); ++i) {
            if (tx->vout[i].scriptPubKey == script && tx->vout[i].nValue == amount) return COutPoint{tx->GetHash(), i};
        }
        BOOST_FAIL("no such output");
        return COutPoint{};
    }

    bool Spent(const COutPoint& outpoint) { return WITH_LOCK(wallet->cs_wallet, return wallet->IsSpent(outpoint.hash, outpoint.n)); }

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
        tallyItem.nRankAmount = tallyItem.nAmount; // ordinary coins only: the rank is their sum
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

// CJ4-B2b2: the selector lists the wallet's coins for a DFCN-scale bit (5-7), as for an old one (CJ4-A turned these bits
// away), while an nDenom that is no single bit of the wire table returns before any coin is listed, as in v23.0.0.
// CWallet is final, so the listing is observed through its COINJOIN log line.
BOOST_FIXTURE_TEST_CASE(coinjoin_selector_lists_coins_for_dfcn_scale_bits, CTransactionBuilderTestSetup)
{
    CoinJoinLogLines log;
    const std::string listing{"CWallet::SelectTxDSInsByDenomination -- vCoins.size():"};

    std::vector<CTxDSIn> selected;
    // An old bit lists the wallet's ready-to-mix coins and logs how many there are.
    wallet->SelectTxDSInsByDenomination(1 << 0, MAX_MONEY, selected);
    BOOST_CHECK(log.Logged(listing));
    for (const int bit : {1 << 5, 1 << 6, 1 << 7}) {
        BOOST_CHECK(!wallet->SelectTxDSInsByDenomination(bit, MAX_MONEY, selected)); // the fixture has no such coin
        BOOST_CHECK(selected.empty());
        BOOST_CHECK_MESSAGE(log.Logged(listing), "a DFCN-scale bit lists the wallet's coins");
    }
    for (const int nDenom : {0, (1 << 0) | (1 << 1), 1 << 8}) {
        BOOST_CHECK(!wallet->SelectTxDSInsByDenomination(nDenom, MAX_MONEY, selected));
        BOOST_CHECK_MESSAGE(!log.Logged(listing), "an nDenom of no single bit returns before the wallet lists its coins");
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
        // The old client never funds from a DFCN-scale candidate, mixed or not (CJ4-B2b): locking the fully mixed coin
        // leaves both anonymizable balances as they were, while the wallet's ordinary funds still count.
        const auto funding = [&] {
            return std::make_pair(wallet->GetAnonymizableBalance(/*fSkipDenominated=*/false),
                                  wallet->GetAnonymizableBalance(/*fSkipDenominated=*/true));
        };
        const auto with_coin{funding()};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->LockCoin(chain.prevout)));
        const auto without_coin{funding()};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->UnlockCoin(chain.prevout)));
        BOOST_CHECK(with_coin.second > 0);
        BOOST_CHECK_MESSAGE(with_coin == without_coin, "the old client does not fund from a fully mixed DFCN-scale candidate");
        // Nor is it DFCN-scale mixing work (CJ4-B2b2): locking it leaves the work as it was.
        const auto work_with{wallet->GetDfcnScaleMixingWork()};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->LockCoin(chain.prevout)));
        const auto work_without{wallet->GetDfcnScaleMixingWork()};
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->UnlockCoin(chain.prevout)));
        BOOST_CHECK_MESSAGE(work_with == work_without, "a fully mixed DFCN-scale coin is no mixing work");
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

    // CJ4-B2b2: the DFCN-scale selector keeps the budget per round count, with each coin's own count: a 0-round and a
    // 1-round coin of one amount, neither fully mixed (16 rounds wanted here), every other coin locked; a budget of one
    // coin returns both, each with its count. A common budget, or every DFCN-scale coin read as round 0, returns one.
    {
        CCoinJoinClientOptions::SetRounds(16);
        const In zero{coin(new_key(true), amount)};
        const In one{mix_once(coin(new_key(true), amount), new_key(true))};
        BOOST_REQUIRE(rounds(zero.prevout) == 0 && rounds(one.prevout) == 1);
        LockAllBut({zero.prevout, one.prevout});
        std::vector<CTxDSIn> selected;
        BOOST_CHECK(wallet->SelectTxDSInsByDenomination(1 << 5, amount, selected));
        std::map<COutPoint, int> by_outpoint;
        for (const auto& in : selected) by_outpoint.emplace(in.prevout, in.nRounds);
        BOOST_CHECK_MESSAGE(by_outpoint == (std::map<COutPoint, int>{{zero.prevout, 0}, {one.prevout, 1}}),
                            "the DFCN-scale selector keeps the budget per round count");
        WITH_LOCK(wallet->cs_wallet, wallet->UnlockAllCoins());
        CCoinJoinClientOptions::SetRounds(1);
    }

    // With CoinJoin disabled the DFCN-scale sums are zero, as GetBalance's mixing fields are (the salt guards read both).
    CCoinJoinClientOptions::SetEnabled(false);
    {
        CAmount dfcn_mixed{-1}, dfcn_all{-1};
        wallet->GetBalance(0, true, false, nullptr, &dfcn_mixed, &dfcn_all);
        BOOST_CHECK_MESSAGE(dfcn_mixed == 0 && dfcn_all == 0, "with CoinJoin disabled the balance has no DFCN-scale coins");
    }
}

// CJ4-B2b1: the old client never funds from a DFCN-scale candidate (a P2PKH output of exactly a DFCN-scale amount whose
// key the wallet holds). The tally keeps a candidate out of what its group may spend and in the group's rank, the total
// v23.0.0 computes: the spendable coins and the balances are v23.0.0's for the wallet without its candidates (the
// control locks them), the rank is v23.0.0's for the wallet with them, each under v23.0.0's cut of 400 coins per address.
BOOST_FIXTURE_TEST_CASE(coinjoin_tally_leaves_dfcn_scale_candidates_out, CTransactionBuilderTestSetup)
{
    struct RestoreEnabled {
        const bool before{CCoinJoinClientOptions::IsEnabled()};
        ~RestoreEnabled() { CCoinJoinClientOptions::SetEnabled(before); }
    } restore_enabled;
    CCoinJoinClientOptions::SetEnabled(true);
    const CAmount amount{CoinJoin::DenominationToAmount(1 << 5)}; // 100.001
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };
    const auto groups = [&](bool anonymizable) {
        std::map<CTxDestination, CompactTallyItem> ret;
        for (const auto& item : wallet->SelectCoinsGroupedByAddresses(/*fSkipDenominated=*/false, anonymizable,
                                                                      /*fSkipUnconfirmed=*/true,
                                                                      anonymizable ? -1 : 400)) {
            ret.emplace(item.txdest, item);
        }
        return ret;
    };
    const auto members = [](const CompactTallyItem& item) {
        std::set<COutPoint> ret;
        for (const auto& coin : item.vecInputCoins) ret.insert(coin.outpoint);
        return ret;
    };

    // A: a candidate and 3 DFCN; B: a candidate alone; C: a P2PK 100.001 (no candidate, r13/R8) and 2 DFCN.
    const CKey a{NewWalletKey()}, b{NewWalletKey()}, c{NewWalletKey()};
    const CScript p2pk_c_script{GetScriptForRawPubKey(c.GetPubKey())};
    const CTransactionRef pay{PayOutputs({{p2pkh(a), amount}, {p2pkh(a), 3 * COIN}, {p2pkh(b), amount},
                                          {p2pk_c_script, amount}, {p2pkh(c), 2 * COIN}}, true)};
    const COutPoint cand_a{OutputOf(pay, p2pkh(a), amount)}, ord_a{OutputOf(pay, p2pkh(a), 3 * COIN)},
        cand_b{OutputOf(pay, p2pkh(b), amount)}, p2pk_c{OutputOf(pay, p2pk_c_script, amount)},
        ord_c{OutputOf(pay, p2pkh(c), 2 * COIN)};
    const CTxDestination dest_a{PKHash(a.GetPubKey())}, dest_b{PKHash(b.GetPubKey())}, dest_c{PKHash(c.GetPubKey())};
    const auto balances = [&] {
        return std::make_pair(wallet->GetAnonymizableBalance(/*fSkipDenominated=*/false),
                              wallet->GetAnonymizableBalance(/*fSkipDenominated=*/true));
    };

    LockAllBut({cand_a, ord_a, cand_b, p2pk_c, ord_c});
    std::map<bool, std::map<CTxDestination, CompactTallyItem>> with;
    for (const bool anonymizable : {false, true}) {
        const auto& g{with[anonymizable] = groups(anonymizable)};
        BOOST_REQUIRE(g.size() == 3 && g.count(dest_a) && g.count(dest_b) && g.count(dest_c));
        // A candidate counts toward its group's rank, never toward what the group may spend.
        BOOST_CHECK(members(g.at(dest_a)) == std::set<COutPoint>{ord_a});
        BOOST_CHECK_EQUAL(g.at(dest_a).nAmount, 3 * COIN);
        BOOST_CHECK_EQUAL(g.at(dest_a).nRankAmount, 3 * COIN + amount);
        // A group of candidates only is listed with its rank, so the sort sees v23.0.0's list, and has nothing to spend.
        BOOST_CHECK(g.at(dest_b).vecInputCoins.empty());
        BOOST_CHECK_EQUAL(g.at(dest_b).nAmount, 0);
        BOOST_CHECK_EQUAL(g.at(dest_b).nRankAmount, amount);
        // A P2PK coin of a DFCN-scale amount is no candidate: funds, as in v23.0.0.
        BOOST_CHECK(members(g.at(dest_c)) == (std::set<COutPoint>{p2pk_c, ord_c}));
        BOOST_CHECK_EQUAL(g.at(dest_c).nAmount, amount + 2 * COIN);
        BOOST_CHECK_EQUAL(g.at(dest_c).nRankAmount, amount + 2 * COIN);
    }
    const auto with_balances{balances()};
    BOOST_CHECK_EQUAL(with_balances.first, 3 * COIN + amount + 2 * COIN);
    BOOST_CHECK_EQUAL(with_balances.second, 3 * COIN + amount + 2 * COIN); // the P2PK coin funds the creator's list too

    // Five-amount control, the wallet without its candidates: the same spendable coins and balances, each rank its sum.
    WITH_LOCK(wallet->cs_wallet, wallet->LockCoin(cand_a); wallet->LockCoin(cand_b));
    for (const bool anonymizable : {false, true}) {
        const auto g{groups(anonymizable)};
        BOOST_REQUIRE(g.size() == 2 && g.count(dest_a) && g.count(dest_c));
        for (const auto& dest : {dest_a, dest_c}) {
            BOOST_CHECK(members(g.at(dest)) == members(with[anonymizable].at(dest)));
            BOOST_CHECK_EQUAL(g.at(dest).nAmount, with[anonymizable].at(dest).nAmount);
            BOOST_CHECK_EQUAL(g.at(dest).nRankAmount, g.at(dest).nAmount);
        }
    }
    BOOST_CHECK_MESSAGE(balances() == with_balances, "the balances are v23.0.0's for the wallet without its candidates");

    // r19/R3: the spendable list is built without the candidate before the cut, so a candidate followed by 400 coins at
    // one address leaves 400 inputs, not 399, while the rank is v23.0.0's: the candidate and 399 coins.
    const CKey d{NewWalletKey()}, e{NewWalletKey()};
    const CAmount piece{COIN / 10};
    WITH_LOCK(wallet->cs_wallet, wallet->UnlockAllCoins()); // the fixture's funds pay again
    const CTransactionRef fund{PayOutputs({{p2pkh(d), amount + 400 * piece}}, true)};
    CMutableTransaction mtx;
    mtx.vin.emplace_back(OutputOf(fund, p2pkh(d), amount + 400 * piece));
    mtx.vout.emplace_back(amount, p2pkh(e)); // first in the tally's walk over the transaction's outputs
    for (int i = 0; i < 400; ++i) mtx.vout.emplace_back(piece, p2pkh(e));
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->SignTransaction(mtx)));
    const CTransactionRef fan{MakeTransactionRef(mtx)};
    {
        LOCK2(wallet->cs_wallet, cs_main);
        wallet->CommitTransaction(fan, {}, {});
    }
    Confirm(fan);
    std::set<COutPoint> fan_outputs;
    for (uint32_t i = 0; i < fan->vout.size(); ++i) fan_outputs.emplace(fan->GetHash(), i);
    LockAllBut(fan_outputs);
    const CTxDestination dest_e{PKHash(e.GetPubKey())};
    const auto cut{groups(/*anonymizable=*/false)};
    BOOST_REQUIRE(cut.size() == 1 && cut.count(dest_e));
    BOOST_CHECK_EQUAL(cut.at(dest_e).vecInputCoins.size(), 400U);
    BOOST_CHECK(!members(cut.at(dest_e)).count(COutPoint{fan->GetHash(), 0}));
    BOOST_CHECK_EQUAL(cut.at(dest_e).nAmount, 400 * piece);
    BOOST_CHECK_EQUAL(cut.at(dest_e).nRankAmount, amount + 399 * piece);
    // A cut of 0 lets each count take its first coin, as v23.0.0 lets a new group take its first: the rank the candidate,
    // the spendable list the first coin of 0.1 DFCN, as for the wallet without the candidate.
    const auto first{wallet->SelectCoinsGroupedByAddresses(/*fSkipDenominated=*/false, /*fAnonymizable=*/false,
                                                           /*fSkipUnconfirmed=*/true, /*nMaxOupointsPerAddress=*/0)};
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_CHECK(members(first[0]) == std::set<COutPoint>{COutPoint(fan->GetHash(), 1)});
    BOOST_CHECK_EQUAL(first[0].nAmount, piece);
    BOOST_CHECK_EQUAL(first[0].nRankAmount, amount);
    // Five-amount control: without the candidate the same 400 coins, ranked by their sum.
    WITH_LOCK(wallet->cs_wallet, wallet->LockCoin(COutPoint{fan->GetHash(), 0}));
    const auto cut_without{groups(/*anonymizable=*/false)};
    BOOST_REQUIRE(cut_without.count(dest_e));
    BOOST_CHECK(members(cut_without.at(dest_e)) == members(cut.at(dest_e)));
    BOOST_CHECK_EQUAL(cut_without.at(dest_e).nRankAmount, 400 * piece);
}

// CJ4-B2b1: the collateral maker and the denomination creator try the groups in v23.0.0's order, ranked with the
// candidates, and never spend a candidate (design note § 2, § 8, § 9.4). Each scenario runs on its own coins, every other
// coin locked; a session of the fixture's wallet without a masternode.
BOOST_FIXTURE_TEST_CASE(coinjoin_old_makers_leave_dfcn_scale_candidates, CTransactionBuilderTestSetup)
{
    struct RestoreEnabled {
        const bool before{CCoinJoinClientOptions::IsEnabled()};
        ~RestoreEnabled() { CCoinJoinClientOptions::SetEnabled(before); }
    } restore_enabled;
    CCoinJoinClientOptions::SetEnabled(true);
    const CAmount amount{CoinJoin::DenominationToAmount(1 << 5)};    // 100.001
    const CAmount small{CoinJoin::DenominationToAmount(1 << 4)};     // 0.00100001, an old denomination
    const CAmount one{CoinJoin::DenominationToAmount(1 << 1)};       // 1.00001, an old denomination
    const CAmount tiny{COIN / 2000};                                 // 0.0005
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };

    // r17/R2, twice: F a candidate, a small denomination and 0.0005; G the recipe's 0.01 DFCN at an address of its own.
    // r18/R1: X a candidate alone, Y 110 DFCN and a 1.00001 denomination, Z 150 DFCN; X2, Y2, Z2 the same with a P2PK
    // 100.001, which every path treats as ordinary funds, in the candidate's place: the base (v23.0.0) emulated.
    // For the creator, twice: J a candidate and 20 DFCN, L 50 DFCN; P a P2PK 100.001 alone (r13/R8). K a candidate
    // alone; K2 a candidate and 0.0005.
    const CKey f0{NewWalletKey()}, g0{NewWalletKey()}, f1{NewWalletKey()}, g1{NewWalletKey()}, x{NewWalletKey()},
        y{NewWalletKey()}, z{NewWalletKey()}, x2{NewWalletKey()}, y2{NewWalletKey()}, z2{NewWalletKey()},
        j0{NewWalletKey()}, l0{NewWalletKey()}, j1{NewWalletKey()}, l1{NewWalletKey()}, p{NewWalletKey()},
        k{NewWalletKey()}, k2{NewWalletKey()};
    const CScript p2pk_x2{GetScriptForRawPubKey(x2.GetPubKey())}, p2pk_p{GetScriptForRawPubKey(p.GetPubKey())};
    const CTransactionRef pay{PayOutputs({{p2pkh(f0), amount}, {p2pkh(f0), small}, {p2pkh(f0), tiny}, {p2pkh(g0), COIN / 100},
                                          {p2pkh(f1), amount}, {p2pkh(f1), small}, {p2pkh(f1), tiny}, {p2pkh(g1), COIN / 100},
                                          {p2pkh(x), amount}, {p2pkh(y), 110 * COIN}, {p2pkh(y), one}, {p2pkh(z), 150 * COIN},
                                          {p2pk_x2, amount}, {p2pkh(y2), 110 * COIN}, {p2pkh(y2), one}, {p2pkh(z2), 150 * COIN},
                                          {p2pkh(j0), amount}, {p2pkh(j0), 20 * COIN}, {p2pkh(l0), 50 * COIN},
                                          {p2pkh(j1), amount}, {p2pkh(j1), 20 * COIN}, {p2pkh(l1), 50 * COIN},
                                          {p2pk_p, amount}, {p2pkh(k), amount}, {p2pkh(k2), amount}, {p2pkh(k2), tiny}},
                                         true)};
    const auto out = [&](const CKey& key, CAmount value) { return OutputOf(pay, p2pkh(key), value); };
    CoinJoinLogLines log;

    BOOST_REQUIRE(m_node.cj_ctx->walletman->DoForClient(wallet->GetName(), [&](CCoinJoinClientManager& clientman) {
        CCoinJoinClientSession session(wallet, *m_node.cj_ctx->walletman, clientman, *m_node.dmnman, *m_node.mn_metaman,
                                       *m_node.mn_sync, *m_node.llmq_ctx->isman, m_node.cj_ctx->queueman,
                                       /*is_masternode=*/false);

        // r17/R2: F ranks above G with its candidate, so the maker spends the 0.01 and the small denomination stays.
        LockAllBut({out(f0, amount), out(f0, small), out(f0, tiny), out(g0, COIN / 100)});
        BOOST_CHECK(MakeCollateralAmountsForTest(session));
        BOOST_CHECK(Spent(out(g0, COIN / 100)));
        BOOST_CHECK_MESSAGE(!Spent(out(f0, small)) && !Spent(out(f0, tiny)) && !Spent(out(f0, amount)),
                            "the maker tries the groups in v23.0.0's order, the candidate counted");
        // Control, the wallet without its candidate: F is the smaller group, and v23.0.0 spends its small denomination.
        LockAllBut({out(f1, small), out(f1, tiny), out(g1, COIN / 100)});
        BOOST_CHECK(MakeCollateralAmountsForTest(session));
        BOOST_CHECK(Spent(out(f1, small)) && Spent(out(f1, tiny)));
        BOOST_CHECK(!Spent(out(g1, COIN / 100)));

        // r18/R1, the stated residual (§ 8): the base makes the collateral from X, the smallest group, and keeps Y's old
        // denomination; with X a candidate group with nothing to spend, the maker spends Y, its denomination with it,
        // while the candidate and Z's ordinary funds stay untouched.
        LockAllBut({OutputOf(pay, p2pk_x2, amount), out(y2, 110 * COIN), out(y2, one), out(z2, 150 * COIN)});
        BOOST_CHECK(MakeCollateralAmountsForTest(session));
        BOOST_CHECK_MESSAGE(Spent(OutputOf(pay, p2pk_x2, amount)) && !Spent(out(y2, one)) && !Spent(out(y2, 110 * COIN)) &&
                                !Spent(out(z2, 150 * COIN)),
                            "the base makes the collateral from X and keeps Y's old denomination");
        LockAllBut({out(x, amount), out(y, 110 * COIN), out(y, one), out(z, 150 * COIN)});
        BOOST_CHECK(MakeCollateralAmountsForTest(session));
        BOOST_CHECK_MESSAGE(Spent(out(y, one)) && Spent(out(y, 110 * COIN)) && !Spent(out(x, amount)) &&
                                !Spent(out(z, 150 * COIN)),
                            "with X's candidate left out the maker spends Y's old denomination, as § 8 states");

        // The creator starts from the largest group as v23.0.0 ranks it: J with its candidate ranks above L, so it funds
        // denominations from J's 20 DFCN, never from the candidate (at every height, r13/R5), and L stays untouched.
        LockAllBut({out(j0, amount), out(j0, 20 * COIN), out(l0, 50 * COIN)});
        BOOST_CHECK(CreateDenominatedForTest(session, 15 * COIN));
        BOOST_CHECK_MESSAGE(Spent(out(j0, 20 * COIN)) && !Spent(out(l0, 50 * COIN)),
                            "the creator tries the groups in v23.0.0's order, the candidate counted");
        BOOST_CHECK_MESSAGE(!Spent(out(j0, amount)), "the creator never spends a candidate");
        // Control, the wallet without its candidate: L is the larger group.
        LockAllBut({out(j1, 20 * COIN), out(l1, 50 * COIN)});
        BOOST_CHECK(CreateDenominatedForTest(session, 15 * COIN));
        BOOST_CHECK(Spent(out(l1, 50 * COIN)) && !Spent(out(j1, 20 * COIN)));
        // A P2PK coin of a DFCN-scale amount is no candidate: the creator funds old denominations from it, as v23.0.0.
        LockAllBut({OutputOf(pay, p2pk_p, amount)});
        BOOST_CHECK(CreateDenominatedForTest(session, 15 * COIN));
        BOOST_CHECK_MESSAGE(Spent(OutputOf(pay, p2pk_p, amount)), "the creator funds from a P2PK 100.001, as v23.0.0");

        // A wallet whose only coin is a candidate: the maker tries no group, as v23.0.0 for the wallet without it.
        LockAllBut({out(k, amount)});
        log.Logged(""); // forget the lines so far
        BOOST_CHECK(!MakeCollateralAmountsForTest(session));
        BOOST_CHECK_MESSAGE(log.Logged("MakeCollateralAmounts -- SelectCoinsGroupedByAddresses can't find any inputs!"),
                            "the maker finds no group with anything to spend");
        BOOST_CHECK(!Spent(out(k, amount)));
        // A candidate and 0.0005 DFCN: the group ranks above the smallest denomination but has less than it to spend, a
        // group v23.0.0 would not list for the wallet without the candidate, so the creator tries none either.
        LockAllBut({out(k2, amount), out(k2, tiny)});
        BOOST_CHECK(!CreateDenominatedForTest(session, 15 * COIN));
        BOOST_CHECK_MESSAGE(log.Logged("CreateDenominated -- SelectCoinsGroupedByAddresses can't find any inputs!"),
                            "the creator finds no group with anything to spend");
        BOOST_CHECK(!Spent(out(k2, amount)) && !Spent(out(k2, tiny)));
    }));
}

// CJ4-B2b2: the wallet side of the DFCN-scale attempt, which no caller uses yet. For a DFCN-scale bit the selector takes
// durable candidates of that amount within the budget per round count and each script once (B1a's and B1b's rules); the
// work function sums each bit's durable candidates that are not fully mixed; neither asks a coin of the five old amounts
// about its rounds, so the old round cache's queries stay v23.0.0's. Five-amount control: an old bit asks the old coin.
BOOST_FIXTURE_TEST_CASE(coinjoin_dfcn_scale_selector_and_work, CTransactionBuilderTestSetup)
{
    struct RestoreEnabled {
        const bool before{CCoinJoinClientOptions::IsEnabled()};
        ~RestoreEnabled() { CCoinJoinClientOptions::SetEnabled(before); }
    } restore_enabled;
    CCoinJoinClientOptions::SetEnabled(true);
    const CAmount amount{CoinJoin::DenominationToAmount(1 << 5)};     // 100.001
    const CAmount amount6{CoinJoin::DenominationToAmount(1 << 6)};    // 1000.01
    const CAmount old_amount{CoinJoin::DenominationToAmount(1 << 0)}; // 10.0001
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };

    // A: two 100.001 to one script; B, C: one each; D: 1000.01; E: 100.001 the user locks; P: a P2PK 100.001; O: an old
    // 10.0001. F: 100.001 in the mempool, neither confirmed nor InstantSend-locked. The selector takes one coin per
    // transaction (inherited), so A's coins, B and C come in transactions of their own.
    const CKey a{NewWalletKey()}, b{NewWalletKey()}, c{NewWalletKey()}, d{NewWalletKey()}, e{NewWalletKey()},
        p{NewWalletKey()}, o{NewWalletKey()}, f{NewWalletKey()};
    const CScript p2pk_p{GetScriptForRawPubKey(p.GetPubKey())};
    const auto pay_one = [&](const CScript& script, CAmount value) {
        return OutputOf(PayOutputs({{script, value}}, true), script, value);
    };
    const std::set<COutPoint> candidates_a{pay_one(p2pkh(a), amount), pay_one(p2pkh(a), amount)};
    BOOST_REQUIRE_EQUAL(candidates_a.size(), 2U);
    const COutPoint coin_b{pay_one(p2pkh(b), amount)}, coin_c{pay_one(p2pkh(c), amount)};
    const CTransactionRef pay{PayOutputs({{p2pkh(d), amount6}, {p2pkh(e), amount}, {p2pk_p, amount}, {p2pkh(o), old_amount}},
                                         true)};
    const COutPoint coin_d{OutputOf(pay, p2pkh(d), amount6)}, coin_e{OutputOf(pay, p2pkh(e), amount)},
        old_coin{OutputOf(pay, p2pkh(o), old_amount)};
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->LockCoin(coin_e)));
    PayOutputs({{p2pkh(f), amount}}, false);

    // The work: A's two coins, B and C for bit 5, D for bit 6, nothing for bit 7; not the locked, the P2PK or the
    // mempool coin.
    BOOST_CHECK_MESSAGE(wallet->GetDfcnScaleMixingWork() == (std::array<CAmount, 3>{4 * amount, amount6, 0}),
                        "the work is each bit's durable candidates");

    // The selector for bit 5: one of A's coins, B and C (a script once per round count), within the budget.
    std::vector<CTxDSIn> selected;
    for (const auto& [budget, expected] : std::vector<std::pair<CAmount, size_t>>{{MAX_MONEY, 3}, {3 * amount, 3}, {2 * amount, 2}}) {
        BOOST_CHECK(wallet->SelectTxDSInsByDenomination(1 << 5, budget, selected));
        std::set<CScript> scripts;
        for (const auto& in : selected) {
            BOOST_CHECK(candidates_a.count(in.prevout) || in.prevout == coin_b || in.prevout == coin_c);
            BOOST_CHECK_EQUAL(in.nRounds, 0);
            scripts.insert(in.prevPubKey);
        }
        BOOST_CHECK_MESSAGE(selected.size() == expected && scripts.size() == expected,
                            "the DFCN-scale selector keeps the budget and takes a script once");
    }
    BOOST_CHECK(wallet->SelectTxDSInsByDenomination(1 << 6, MAX_MONEY, selected));
    BOOST_CHECK(selected.size() == 1 && selected[0].prevout == coin_d);
    BOOST_CHECK(!wallet->SelectTxDSInsByDenomination(1 << 7, MAX_MONEY, selected));

    // No question to the old round cache: after clearing it, the work and a DFCN-scale selection log no round count for
    // the old coin; an old bit's selection does (the control).
    CoinJoinLogLines log;
    wallet->ClearCoinJoinRoundsCache();
    log.Logged(""); // forget the lines so far
    (void)wallet->GetDfcnScaleMixingWork();
    wallet->SelectTxDSInsByDenomination(1 << 5, MAX_MONEY, selected);
    BOOST_CHECK_MESSAGE(!log.Logged(old_coin.ToStringShort()), "the DFCN-scale work and selector do not ask an old coin");
    wallet->SelectTxDSInsByDenomination(1 << 0, MAX_MONEY, selected);
    BOOST_CHECK_MESSAGE(log.Logged(old_coin.ToStringShort()), "an old bit asks the old coin about its rounds");
    // The flag keeps ONLY_FULLY_MIXED off the old coins as well.
    wallet->ClearCoinJoinRoundsCache();
    log.Logged("");
    {
        std::vector<COutput> coins;
        CCoinControl coin_control;
        coin_control.nCoinType = CoinType::ONLY_FULLY_MIXED;
        coin_control.m_dfcn_scale_only = true;
        LOCK(wallet->cs_wallet);
        wallet->AvailableCoins(coins, &coin_control);
    }
    BOOST_CHECK_MESSAGE(!log.Logged(old_coin.ToStringShort()), "ONLY_FULLY_MIXED with the flag does not ask an old coin");

    // No cut in the work: six more bit-5 candidates make ten, past one session's nine.
    PayOutputs({{p2pkh(NewWalletKey()), amount}, {p2pkh(NewWalletKey()), amount}, {p2pkh(NewWalletKey()), amount},
                {p2pkh(NewWalletKey()), amount}, {p2pkh(NewWalletKey()), amount}, {p2pkh(NewWalletKey()), amount}},
               true);
    BOOST_CHECK_MESSAGE(wallet->GetDfcnScaleMixingWork()[0] == 10 * amount, "the work has no cut");
}

// CJ4-B2b1: the old client's queue walk asks for the five bits of v23.0.0. A queue of a DFCN-scale bit stays untried; an
// nDenom that is no single bit of the wire table is tried and turned away as in v23.0.0, and so is an old bit.
BOOST_AUTO_TEST_CASE(coinjoin_old_queue_walk_leaves_dfcn_scale_queues)
{
    class Queues : public CCoinJoinBaseManager
    {
    public:
        void Add(int nDenom) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
        {
            LOCK(cs_vecqueue);
            vecCoinJoinQueue.emplace_back(nDenom, COutPoint{uint256::ONE, uint32_t(vecCoinJoinQueue.size())}, uint256{},
                                          GetAdjustedTime(), /*fReady=*/false);
        }
        bool Tried(int nDenom) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
        {
            LOCK(cs_vecqueue);
            for (const auto& dsq : vecCoinJoinQueue) {
                if (dsq.nDenom == nDenom) return dsq.fTried;
            }
            BOOST_FAIL("no queue of this nDenom");
            return false;
        }
    } queues;
    const int several{(1 << 0) | (1 << 1)};
    for (const int nDenom : {1 << 5, several, 1 << 0, 1 << 7, 1 << 6}) queues.Add(nDenom);

    std::vector<int> walked;
    CCoinJoinQueue dsq;
    while (queues.GetQueueItemAndTry(dsq)) walked.push_back(dsq.nDenom);
    BOOST_CHECK(walked == (std::vector<int>{several, 1 << 0}));
    BOOST_CHECK(queues.Tried(several) && queues.Tried(1 << 0));
    for (const int bit : {1 << 5, 1 << 6, 1 << 7}) {
        BOOST_CHECK_MESSAGE(!queues.Tried(bit), "the old walk leaves a DFCN-scale queue untried");
    }

    // CJ4-B2b3: the DFCN-scale attempt's walk takes, of the queues not tried, only one of a bit it asks for. The bit-5 and
    // bit-7 queues the old walk left come back; the bit-6 queue, not asked for, stays, and so do a queue of two DFCN-scale
    // bits, an nDenom past the table and an old bit, which the old walk then tries as in v23.0.0.
    const int several_new{(1 << 5) | (1 << 7)};
    for (const int nDenom : {several_new, 1 << 8, 1 << 1}) queues.Add(nDenom);
    walked.clear();
    while (queues.GetQueueItemAndTry(dsq, (1 << 5) | (1 << 7))) walked.push_back(dsq.nDenom);
    BOOST_CHECK_MESSAGE(walked == (std::vector<int>{1 << 5, 1 << 7}), "the DFCN-scale walk takes the queues of its bits only");
    BOOST_CHECK(!queues.Tried(1 << 6) && !queues.Tried(several_new) && !queues.Tried(1 << 8) && !queues.Tried(1 << 1));
    walked.clear();
    while (queues.GetQueueItemAndTry(dsq)) walked.push_back(dsq.nDenom);
    BOOST_CHECK(walked == (std::vector<int>{several_new, 1 << 8, 1 << 1}));
    BOOST_CHECK(!queues.Tried(1 << 6));
}

// CJ4-B2b3: which attempt (design note § 2). From U, with work, the DFCN-scale attempt runs where the old path would stop,
// and otherwise by a coin toss, which is made only then; a new DFCN-scale queue takes a bit with work, any of them.
BOOST_AUTO_TEST_CASE(coinjoin_dfcn_scale_attempt_choice)
{
    int tosses{0};
    const auto toss = [&tosses](bool result) {
        return std::function<bool()>{[&tosses, result] {
            ++tosses;
            return result;
        }};
    };
    for (const bool old_would_stop : {true, false}) {
        for (const bool result : {true, false}) {
            BOOST_CHECK(!CoinJoin::ChooseDfcnScaleAttempt(/*open=*/false, /*has_work=*/true, old_would_stop, toss(result)));
            BOOST_CHECK(!CoinJoin::ChooseDfcnScaleAttempt(/*open=*/true, /*has_work=*/false, old_would_stop, toss(result)));
            BOOST_CHECK(!CoinJoin::ChooseDfcnScaleAttempt(/*open=*/false, /*has_work=*/false, old_would_stop, toss(result)));
        }
    }
    BOOST_CHECK_MESSAGE(tosses == 0, "no toss when closed or without work");
    BOOST_CHECK(CoinJoin::ChooseDfcnScaleAttempt(true, true, /*old_would_stop=*/true, toss(false)));
    BOOST_CHECK_MESSAGE(tosses == 0, "where the old path stops the DFCN-scale attempt runs without a toss");
    BOOST_CHECK(CoinJoin::ChooseDfcnScaleAttempt(true, true, /*old_would_stop=*/false, toss(true)));
    BOOST_CHECK(!CoinJoin::ChooseDfcnScaleAttempt(true, true, /*old_would_stop=*/false, toss(false)));
    BOOST_CHECK_MESSAGE(tosses == 2, "where the old path goes on one toss decides");

    BOOST_CHECK_EQUAL(CoinJoin::DfcnScaleWorkBits({0, 0, 0}), 0);
    BOOST_CHECK_EQUAL(CoinJoin::DfcnScaleWorkBits({1, 0, 1}), (1 << 5) | (1 << 7));
    BOOST_CHECK_EQUAL(CoinJoin::DfcnScaleWorkBits({0, 1, 0}), 1 << 6);
    BOOST_CHECK_EQUAL(CoinJoin::RandomDfcnScaleWorkBit({0, 0, 0}), 0);
    std::set<int> seen;
    for (int i = 0; i < 200; ++i) seen.insert(CoinJoin::RandomDfcnScaleWorkBit({1, 0, 1}));
    BOOST_CHECK_MESSAGE(seen == (std::set<int>{1 << 5, 1 << 7}), "the new queue takes a bit with work, any of them");
    for (int i = 0; i < 20; ++i) BOOST_CHECK_EQUAL(CoinJoin::RandomDfcnScaleWorkBit({0, 1, 0}), 1 << 6);
}

// CJ4-B2b3: the DFCN-scale attempt's collateral maker runs v23.0.0's first pass in rank order over the address groups that
// hold no old denomination: it skips a smaller group with an old denomination, which v23.0.0's maker spends first (the
// control), and treats a P2PK coin of a DFCN-scale amount, which is no candidate, as ordinary funds.
BOOST_FIXTURE_TEST_CASE(coinjoin_dfcn_scale_maker_spares_old_denominations, CTransactionBuilderTestSetup)
{
    struct RestoreEnabled {
        const bool before{CCoinJoinClientOptions::IsEnabled()};
        ~RestoreEnabled() { CCoinJoinClientOptions::SetEnabled(before); }
    } restore_enabled;
    CCoinJoinClientOptions::SetEnabled(true);
    const CAmount amount{CoinJoin::DenominationToAmount(1 << 5)}; // 100.001
    const CAmount small{CoinJoin::DenominationToAmount(1 << 4)};  // 0.00100001, an old denomination
    const CAmount tiny{COIN / 2000};                              // 0.0005
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };

    // F: a small denomination and 0.0005, the smallest group; G: 0.01 and, for the DFCN-scale maker, a candidate (G ranks
    // 100.011 with 0.01 to spend); H: 0.02; twice (0 for the DFCN-scale maker, 1 for the control). P: a P2PK 100.001
    // alone.
    const CKey f0{NewWalletKey()}, g0{NewWalletKey()}, h0{NewWalletKey()}, f1{NewWalletKey()}, g1{NewWalletKey()},
        h1{NewWalletKey()}, p{NewWalletKey()};
    const CScript p2pk_p{GetScriptForRawPubKey(p.GetPubKey())};
    const CTransactionRef pay{PayOutputs({{p2pkh(f0), small}, {p2pkh(f0), tiny}, {p2pkh(g0), COIN / 100}, {p2pkh(g0), amount},
                                          {p2pkh(h0), COIN / 50}, {p2pkh(f1), small}, {p2pkh(f1), tiny},
                                          {p2pkh(g1), COIN / 100}, {p2pkh(h1), COIN / 50}, {p2pk_p, amount}},
                                         true)};
    const auto out = [&](const CKey& key, CAmount value) { return OutputOf(pay, p2pkh(key), value); };

    BOOST_REQUIRE(m_node.cj_ctx->walletman->DoForClient(wallet->GetName(), [&](CCoinJoinClientManager& clientman) {
        CCoinJoinClientSession session(wallet, *m_node.cj_ctx->walletman, clientman, *m_node.dmnman, *m_node.mn_metaman,
                                       *m_node.mn_sync, *m_node.llmq_ctx->isman, m_node.cj_ctx->queueman,
                                       /*is_masternode=*/false);

        LockAllBut({out(f0, small), out(f0, tiny), out(g0, COIN / 100), out(g0, amount), out(h0, COIN / 50)});
        BOOST_CHECK(MakeCollateralAmountsOrdinaryForTest(session));
        BOOST_CHECK_MESSAGE(!Spent(out(f0, small)) && !Spent(out(f0, tiny)),
                            "the DFCN-scale maker leaves a group with an old denomination out");
        BOOST_CHECK_MESSAGE(Spent(out(h0, COIN / 50)) && !Spent(out(g0, COIN / 100)) && !Spent(out(g0, amount)),
                            "the DFCN-scale maker tries the other groups by rank, the candidate counted");
        // Control: v23.0.0's maker spends F first, its small denomination with it.
        LockAllBut({out(f1, small), out(f1, tiny), out(g1, COIN / 100), out(h1, COIN / 50)});
        BOOST_CHECK(MakeCollateralAmountsForTest(session));
        BOOST_CHECK(Spent(out(f1, small)) && Spent(out(f1, tiny)) && !Spent(out(g1, COIN / 100)));
        // A P2PK coin of a DFCN-scale amount is no candidate: ordinary funds to this maker too.
        LockAllBut({OutputOf(pay, p2pk_p, amount)});
        BOOST_CHECK(MakeCollateralAmountsOrdinaryForTest(session));
        BOOST_CHECK_MESSAGE(Spent(OutputOf(pay, p2pk_p, amount)), "a P2PK 100.001 is ordinary funds to the DFCN-scale maker");
    }));
}

// CJ4-B2b3: the automatic attempt, with the DFCN-scale gate given below its height read. Open (from U) and with work, it is
// the DFCN-scale attempt where the old path stops ("Nothing to do", "Not enough funds to mix"); that attempt is v23.0.0's
// from its collateral step on, with the maker of ordinary funds in groups without an old denomination, and its queue walk
// asks for the bits it has work for. Closed, every attempt is the old one. Design note § 9.4: r13/R3, r16/R1, the recipe's
// 0.01 DFCN, r13/R4, r13/R7, r19/R2.
BOOST_FIXTURE_TEST_CASE(coinjoin_dfcn_scale_attempt, CTransactionBuilderTestSetup)
{
    struct RestoreOptions {
        const bool enabled{CCoinJoinClientOptions::IsEnabled()};
        const int rounds{CCoinJoinClientOptions::GetRounds()};
        const int amount{CCoinJoinClientOptions::GetAmount()};
        const bool multi_session{CCoinJoinClientOptions::IsMultiSessionEnabled()};
        ~RestoreOptions()
        {
            CCoinJoinClientOptions::SetEnabled(enabled);
            CCoinJoinClientOptions::SetRounds(rounds);
            CCoinJoinClientOptions::SetAmount(amount);
            CCoinJoinClientOptions::SetMultiSessionEnabled(multi_session);
        }
    } restore_options;
    CCoinJoinClientOptions::SetEnabled(true);
    CCoinJoinClientOptions::SetRounds(1);
    CCoinJoinClientOptions::SetAmount(MIN_COINJOIN_AMOUNT); // the old target, which one fully mixed 10.0001 reaches
    CCoinJoinClientOptions::SetMultiSessionEnabled(false);
    BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false)); // a new queue's start asserts a loaded cache

    const CAmount amount{CoinJoin::DenominationToAmount(1 << 5)};     // 100.001
    const CAmount old_amount{CoinJoin::DenominationToAmount(1 << 0)}; // 10.0001
    const CAmount small{CoinJoin::DenominationToAmount(1 << 4)};      // 0.00100001
    const auto p2pkh = [](const CKey& key) { return GetScriptForDestination(PKHash(key.GetPubKey())); };

    // M: a fully mixed 10.0001 (four real rounds), with 0.001 DFCN paid to its address later; C: a candidate; R, R2: the
    // recipe's 0.01 DFCN, each at an address of its own; Q: one collateral input; T: 0.0005; S: a small denomination;
    // F: 5 DFCN that later pay an old denomination into the mempool; F2: 2 DFCN that later pay an own collateral into
    // the mempool; X: 0.002 DFCN to a key of another party, which later pays a collateral to the wallet; D: a 1.00001
    // denomination.
    const CKey m{NewWalletKey()}, c{NewWalletKey()}, r{NewWalletKey()}, r2{NewWalletKey()}, q{NewWalletKey()},
        t{NewWalletKey()}, s{NewWalletKey()}, f{NewWalletKey()}, f2{NewWalletKey()}, d{NewWalletKey()};
    CKey x;
    x.MakeNewKey(true);
    const CAmount one{CoinJoin::DenominationToAmount(1 << 1)}; // 1.00001
    const CTransactionRef pay{PayOutputs({{p2pkh(m), old_amount}, {p2pkh(c), amount}, {p2pkh(r), COIN / 100},
                                          {p2pkh(r2), COIN / 100}, {p2pkh(q), CoinJoin::GetMaxCollateralAmount()},
                                          {p2pkh(t), COIN / 2000}, {p2pkh(s), small}, {p2pkh(f), 5 * COIN},
                                          {p2pkh(f2), 2 * COIN}, {p2pkh(x), COIN / 500}, {p2pkh(d), one}},
                                         true)};
    const auto out = [&](const CKey& key, CAmount value) { return OutputOf(pay, p2pkh(key), value); };
    COutPoint mixed{out(m, old_amount)};
    CKey mixed_key;
    for (int round = 1; round <= 4; ++round) {
        mixed_key = NewWalletKey();
        mixed = MixOnce(mixed, old_amount, p2pkh(mixed_key));
    }
    BOOST_REQUIRE_GE(4, 1 + CCoinJoinClientOptions::GetRandomRounds());
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->IsFullyMixed(mixed)));
    // The 0.001 DFCN is paid with the test's coins locked, so that the payment takes none of them.
    {
        LOCK(wallet->cs_wallet);
        for (uint32_t i = 0; i < pay->vout.size(); ++i) wallet->LockCoin(COutPoint{pay->GetHash(), i});
        wallet->LockCoin(mixed);
    }
    const COutPoint mixed_small{OutputOf(PayOutputs({{p2pkh(mixed_key), COIN / 1000}}, true), p2pkh(mixed_key), COIN / 1000)};
    {
        LOCK(wallet->cs_wallet);
        wallet->UnlockAllCoins();
    }
    const COutPoint cand{out(c, amount)}, recipe{out(r, COIN / 100)}, recipe2{out(r2, COIN / 100)},
        collateral{out(q, CoinJoin::GetMaxCollateralAmount())}, tiny{out(t, COIN / 2000)}, small_denom{out(s, small)},
        d_denom{out(d, one)};

    // The session's queues are the test's: their masternodes are in no list, so a walk only marks what it takes.
    class Queues : public CCoinJoinClientQueueManager
    {
    public:
        using CCoinJoinClientQueueManager::CCoinJoinClientQueueManager;
        void Add(int nDenom) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
        {
            LOCK(cs_vecqueue);
            vecCoinJoinQueue.emplace_back(nDenom, COutPoint{uint256::ONE, uint32_t(vecCoinJoinQueue.size())}, uint256{},
                                          GetAdjustedTime(), /*fReady=*/false);
        }
        bool Tried(int nDenom) EXCLUSIVE_LOCKS_REQUIRED(!cs_vecqueue)
        {
            LOCK(cs_vecqueue);
            for (const auto& dsq : vecCoinJoinQueue) {
                if (dsq.nDenom == nDenom) return dsq.fTried;
            }
            BOOST_FAIL("no queue of this nDenom");
            return false;
        }
    };
    Queues queues{*m_node.cj_ctx->walletman, *m_node.dmnman, *m_node.mn_metaman, *m_node.mn_sync, /*is_masternode=*/false};
    // The session reads its queue manager through a unique_ptr; this one only points at `queues` and is released before
    // it goes, so nothing is deleted through the base class (it has no virtual destructor).
    std::unique_ptr<CCoinJoinClientQueueManager> queueman{&queues};
    struct Release {
        std::unique_ptr<CCoinJoinClientQueueManager>& ptr;
        ~Release() { (void)ptr.release(); }
    } release_queueman{queueman};

    // A client manager of the test's own, which lives as long as the session, so no manager borrowed from DoForClient
    // outlives its callback (and the manager map's lock is free while the test mines a block between attempts).
    CCoinJoinClientManager clientman(wallet, *m_node.cj_ctx->walletman, *m_node.dmnman, *m_node.mn_metaman, *m_node.mn_sync,
                                     *m_node.llmq_ctx->isman, queueman, /*is_masternode=*/false);
    CCoinJoinClientSession session(wallet, *m_node.cj_ctx->walletman, clientman, *m_node.dmnman, *m_node.mn_metaman,
                                   *m_node.mn_sync, *m_node.llmq_ctx->isman, queueman, /*is_masternode=*/false);
    CoinJoinLogLines log;
    // The coin toss is the test's: it counts the tosses and answers toss_result.
    int tosses{0};
    bool toss_result{false};
    const auto attempt = [&](bool open) {
        log.Logged(""); // forget the lines so far
        return DoAttemptForTest(session, *m_node.chainman, *m_node.connman, *m_node.mempool, open, [&] {
            ++tosses;
            return toss_result;
        });
    };
    const auto tx_count = [&] { return WITH_LOCK(wallet->cs_wallet, return wallet->mapWallet.size()); };
    const auto locked = [&](const COutPoint& outpoint) {
        return WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(outpoint.hash, outpoint.n));
    };
    const auto spender = [&](const COutPoint& outpoint) {
        LOCK(wallet->cs_wallet);
        for (const auto& [hash, wtx] : wallet->mapWallet) {
            for (const auto& in : wtx.tx->vin) {
                if (in.prevout == outpoint) return wtx.tx;
            }
        }
        BOOST_FAIL("no transaction spends the outpoint");
        return CTransactionRef{};
    };

    // r13/R3: the target reached, the fully mixed 10.0001 and a candidate, no other funds, no collateral. Closed, the old
    // path has nothing to do. Open, the DFCN-scale attempt's maker finds no group it may use, and the 10.0001 stays.
    LockAllBut({mixed, cand});
    size_t txes{tx_count()};
    BOOST_CHECK(!attempt(false));
    BOOST_CHECK(log.Logged("DoAutomaticDenominating -- Nothing to do"));
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(log.Logged("MakeCollateralAmountsOrdinary -- no collateral from ordinary funds"),
                        "the open attempt is the DFCN-scale attempt, whose maker finds nothing");
    BOOST_CHECK_MESSAGE(tx_count() == txes && !Spent(mixed) && !Spent(cand), "the DFCN-scale attempt never breaks an old denomination");

    // r16/R1: 0.001 DFCN at the 10.0001's address, which v23.0.0's first pass would spend with it (the control at the end).
    LockAllBut({mixed, mixed_small, cand});
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(tx_count() == txes && !Spent(mixed) && !Spent(mixed_small),
                        "the DFCN-scale attempt leaves a group with an old denomination out");

    // The recipe's 0.01 DFCN at an address of its own: closed, nothing to do; open, the maker makes the collateral from
    // it, and the attempt ends there.
    LockAllBut({mixed, mixed_small, cand, recipe});
    BOOST_CHECK(!attempt(false));
    BOOST_CHECK_MESSAGE(tx_count() == txes && !Spent(recipe), "a closed attempt is the old one, with nothing to do");
    BOOST_CHECK(attempt(true));
    BOOST_CHECK_MESSAGE(Spent(recipe) && !Spent(mixed) && !Spent(mixed_small) && !Spent(cand),
                        "the DFCN-scale attempt makes the collateral from the recipe's 0.01 DFCN");

    // A candidate the user locked is no work: the open attempt is the old one, with nothing to do.
    LockAllBut({mixed, recipe2});
    txes = tx_count();
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(log.Logged("DoAutomaticDenominating -- Nothing to do") && tx_count() == txes && !Spent(recipe2),
                        "a candidate the user locked gives no DFCN-scale work");

    // r13/R4 and r13/R7, one collateral input Q. The first attempt takes Q for its collateral transaction and walks the
    // queues: it takes the bit-5 queue, which the old walk leaves untried, and leaves the old bit's; no new queue starts,
    // and Q stays locked. The second has no free collateral and makes one from the recipe's second 0.01 DFCN. Once that
    // is mined, the third passes the presence check, keeps its collateral transaction (Q locked again, as in v23.0.0)
    // and reaches the queue search.
    queues.Add(1 << 0);
    queues.Add(1 << 5);
    LockAllBut({mixed, cand, collateral, recipe2});
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK(log.Logged("JoinExistingDfcnScaleQueue -- dsq masternode is not in masternode list"));
    BOOST_CHECK_MESSAGE(queues.Tried(1 << 5) && !queues.Tried(1 << 0),
                        "the DFCN-scale walk takes the bit-5 queue and leaves the old bit's");
    BOOST_CHECK_EQUAL(session.GetStatus(/*fWaitForBlock=*/true).original, "No compatible Masternode found.");
    BOOST_CHECK(tx_count() == txes && locked(collateral) && !Spent(collateral));
    const uint256 kept{CollateralTxHashForTest(session)};
    BOOST_CHECK(attempt(true));
    BOOST_CHECK_MESSAGE(Spent(recipe2) && !Spent(collateral) && locked(collateral) && !Spent(mixed) && !Spent(cand),
                        "without a free collateral the DFCN-scale attempt makes one from the 0.01 DFCN");
    BOOST_REQUIRE_EQUAL(tx_count(), txes + 1);
    const CTransactionRef made{spender(recipe2)};
    Confirm(made);
    txes = tx_count();
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(log.Logged("StartNewDfcnScaleQueue -- Can't find random Masternode."),
                        "with the new collateral the next attempt reaches the queue search");
    BOOST_CHECK_MESSAGE(tx_count() == txes && locked(collateral) && CollateralTxHashForTest(session) == kept,
                        "the third attempt keeps the first one's collateral transaction");
    BOOST_CHECK_MESSAGE(tosses == 0, "the old path's stops take no toss");

    // A new DFCN-scale queue starts only for a bit the wallet can still fill: the work read while C was free, C then
    // locked by the user, the start stops before it looks for a masternode; with C free again it looks for one.
    const auto work{wallet->GetDfcnScaleMixingWork()};
    BOOST_REQUIRE(work[0] == amount);
    WITH_LOCK(wallet->cs_wallet, wallet->LockCoin(cand));
    BOOST_CHECK(!StartNewDfcnScaleQueueForTest(session, work, *m_node.connman));
    BOOST_CHECK_MESSAGE(session.GetStatus(/*fWaitForBlock=*/true).original == "Can't mix: no compatible inputs found!",
                        "a new DFCN-scale queue starts only for a bit the wallet can still fill");
    WITH_LOCK(wallet->cs_wallet, wallet->UnlockCoin(cand));
    BOOST_CHECK(!StartNewDfcnScaleQueueForTest(session, work, *m_node.connman));
    BOOST_CHECK_EQUAL(session.GetStatus(/*fWaitForBlock=*/true).original, "Can't find random Masternode.");

    // r18/R3 and r19/R1: an unconfirmed 0.0004 collateral without an InstantSend lock. Paid by another party (X), it is
    // not present to HasCollateralInputs() but is to HasCollateralInputs(false): the attempt waits, before the maker.
    // Made by the wallet from its own confirmed coins (F2), it is present, and the attempt goes on past its collateral
    // step to the queue search.
    CMutableTransaction ext;
    ext.vin.emplace_back(out(x, COIN / 500));
    const CScript ext_script{p2pkh(NewWalletKey())};
    ext.vout.emplace_back(CoinJoin::GetMaxCollateralAmount(), ext_script);
    FillableSigningProvider x_keys;
    BOOST_REQUIRE(x_keys.AddKey(x));
    BOOST_REQUIRE(SignSignature(x_keys, *pay, ext, 0, SIGHASH_ALL));
    const CTransactionRef ext_tx{MakeTransactionRef(ext)};
    BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ProcessTransaction(ext_tx).m_result_type) ==
                  MempoolAcceptResult::ResultType::VALID);
    wallet->transactionAddedToMempool(ext_tx, /*nAcceptTime=*/GetTime(), /*mempool_sequence=*/0);
    LockAllBut({mixed, cand, OutputOf(ext_tx, ext_script, CoinJoin::GetMaxCollateralAmount())});
    BOOST_REQUIRE(!wallet->HasCollateralInputs() && wallet->HasCollateralInputs(/*fOnlyConfirmed=*/false));
    txes = tx_count();
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(!log.Logged("MakeCollateralAmountsOrdinary") && tx_count() == txes,
                        "an unconfirmed collateral from another party makes the attempt wait before the maker");
    LockAllBut({out(f2, 2 * COIN)});
    const CScript own_script{p2pkh(NewWalletKey())};
    const CTransactionRef own_tx{PayOutputs({{own_script, CoinJoin::GetMaxCollateralAmount()}}, false)};
    LockAllBut({mixed, cand, OutputOf(own_tx, own_script, CoinJoin::GetMaxCollateralAmount())});
    BOOST_REQUIRE(wallet->HasCollateralInputs());
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(session.GetStatus(/*fWaitForBlock=*/true).original == "No compatible Masternode found.",
                        "an unconfirmed collateral the wallet made is present, and the attempt goes on");

    // Plan decision 2: an unconfirmed old denomination of the wallet's own (F pays 1.00001 into the mempool) makes the
    // DFCN-scale attempt wait in single-session mode, as v23.0.0's attempt waits; with several sessions it goes on.
    LockAllBut({out(f, 5 * COIN)});
    PayOutputs({{p2pkh(NewWalletKey()), CoinJoin::DenominationToAmount(1 << 1)}}, false);
    std::set<COutPoint> keep{mixed, cand};
    for (uint32_t i = 0; i < made->vout.size(); ++i) keep.emplace(made->GetHash(), i);
    LockAllBut(keep);
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetBalance().m_denominated_untrusted_pending) > 0);
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(session.GetStatus(/*fWaitForBlock=*/true).original ==
                            "Found unconfirmed denominated outputs, will wait till they confirm to continue.",
                        "the DFCN-scale attempt waits for unconfirmed old denominations too");
    CCoinJoinClientOptions::SetMultiSessionEnabled(true);
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_EQUAL(session.GetStatus(/*fWaitForBlock=*/true).original, "No compatible Masternode found.");
    CCoinJoinClientOptions::SetMultiSessionEnabled(false);

    // Not enough funds to mix (the old target raised, nothing to mix but 0.0005): closed, the old path stops; open, the
    // DFCN-scale attempt makes a collateral from the 0.0005.
    CCoinJoinClientOptions::SetAmount(DEFAULT_COINJOIN_AMOUNT);
    LockAllBut({cand, tiny});
    txes = tx_count();
    BOOST_CHECK(!attempt(false));
    BOOST_CHECK(log.Logged("DoAutomaticDenominating -- Not enough funds to mix.") && tx_count() == txes && !Spent(tiny));
    BOOST_CHECK(attempt(true));
    BOOST_CHECK_MESSAGE(Spent(tiny) && !Spent(cand), "where the old path has not enough funds the DFCN-scale attempt runs");
    BOOST_CHECK_MESSAGE(session.GetStatus(/*fWaitForBlock=*/true).original.empty(),
                        "the DFCN-scale attempt clears an earlier stop status");

    // The coin toss where the old path goes on (the old target not reached, the 1.00001 denomination D to mix, no
    // collateral, a candidate), one toss each: heads, the DFCN-scale attempt, whose maker leaves D's group out; tails,
    // the old path, whose maker breaks D in its second pass, as v23.0.0. (Right after a new transaction, so that the
    // wallet's cached anonymizable groups are this wallet's: a lock does not refresh them, inherited.)
    LockAllBut({cand, d_denom});
    BOOST_CHECK_MESSAGE(tosses == 0, "the not-enough-funds stop takes no toss either");
    toss_result = true;
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(log.Logged("MakeCollateralAmountsOrdinary -- no collateral from ordinary funds") && !Spent(d_denom) &&
                            tosses == 1,
                        "on heads the DFCN-scale attempt runs where the old path goes on");
    toss_result = false;
    BOOST_CHECK(attempt(true));
    BOOST_CHECK_MESSAGE(Spent(d_denom) && !Spent(cand) && tosses == 2, "on tails the old path runs where it goes on");

    // r19/R2: a small old denomination and a candidate. Closed: not enough funds, no maker; open: the DFCN-scale maker
    // leaves the denomination's group out. Either way the denomination stays.
    LockAllBut({cand, small_denom});
    txes = tx_count();
    BOOST_CHECK(!attempt(false));
    BOOST_CHECK(log.Logged("DoAutomaticDenominating -- Not enough funds to mix."));
    BOOST_CHECK(!attempt(true));
    BOOST_CHECK_MESSAGE(log.Logged("MakeCollateralAmountsOrdinary -- no collateral from ordinary funds") &&
                            tx_count() == txes && !Spent(small_denom),
                        "neither attempt breaks the small old denomination, and the open one asks the maker");
    BOOST_CHECK_MESSAGE(tosses == 2, "the two tosses of the third point are the case's only tosses");

    // Control, v23.0.0's maker on the r16/R1 wallet: its first pass spends the 10.0001 with the 0.001 DFCN.
    LockAllBut({mixed, mixed_small});
    BOOST_CHECK(MakeCollateralAmountsForTest(session));
    BOOST_CHECK(Spent(mixed) && Spent(mixed_small));
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
