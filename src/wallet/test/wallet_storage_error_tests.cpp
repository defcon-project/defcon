// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <dbwrapper.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <interfaces/chain.h>
#include <node/context.h>
#include <primitives/block.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/db.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
// DIP0003 active from the next block on (regtest activates it at 432): every block connected from here on stores a
// masternode list diff, which the wallet's own block processing reads (AddToWallet -> LockProTxCoins ->
// listMNCollaterials -> GetListForBlock(tip)). As the EvoDB tests' TestChainDIP3Setup: the fixture checks its
// chain against a regtest checkpoint, which exists for 430 blocks, so the 431st is mined here.
struct WalletDIP3ChainSetup : public TestChainSetup {
    WalletDIP3ChainSetup() : TestChainSetup(430) { CreateAndProcessBlock({}, coinbaseKey); }
};

// As wallet_tests.cpp loads and unloads a wallet on the test data directory: the wallet file persists between the
// two, so a second load reads what the first recorded, as a restart does.
std::shared_ptr<CWallet> LoadTestWallet(NodeContext& node)
{
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto database = MakeWalletDatabase("", options, status, error);
    BOOST_REQUIRE_MESSAGE(database, error.original);
    auto wallet = CWallet::Create(node.chain.get(), node.coinjoin_loader.get(), "", std::move(database), options.create_flags, error, warnings);
    BOOST_REQUIRE_MESSAGE(wallet, error.original);
    AddWallet(wallet);
    return wallet;
}

void UnloadTestWallet(std::shared_ptr<CWallet>&& wallet)
{
    std::vector<bilingual_str> warnings;
    SyncWithValidationInterfaceQueue();
    wallet->m_chain_notifications_handler.reset();
    RemoveWallet(wallet, std::nullopt, warnings);
    UnloadWallet(std::move(wallet));
}

void AddKey(CWallet& wallet, const CKey& key)
{
    auto spk_man = wallet.GetOrCreateLegacyScriptPubKeyMan();
    LOCK2(wallet.cs_wallet, spk_man->cs_KeyStore);
    BOOST_REQUIRE(spk_man->AddKeyPubKey(key, key.GetPubKey()));
}

// The best block the wallet has recorded on disk: where its next start rescans from.
uint256 RecordedBestBlock(CWallet& wallet)
{
    CBlockLocator locator;
    BOOST_REQUIRE(WalletBatch(wallet.GetDatabase()).ReadBestBlock(locator));
    BOOST_REQUIRE(!locator.vHave.empty());
    return locator.vHave.front();
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(wallet_storage_error_tests, WalletDIP3ChainSetup)

// A block the wallet could not process to the end must be rescanned by the next start. The wallet's own block
// processing reads the masternode list history (the collateral check in AddToWallet); a storage error there leaves
// blockConnected by exception before the transaction is written, the node stops in order, and the shutdown's forced
// flush still reaches chainStateFlushed. Had that recorded the tip, the next start would have found nothing to
// rescan and the transaction would have stayed lost. The error is planted as the EvoDB tests plant theirs: the
// block's list diff is erased from the database, and a manager with empty caches reads it.
BOOST_AUTO_TEST_CASE(a_block_the_wallet_could_not_process_is_rescanned_on_the_next_start)
{
    auto& chainman = *Assert(m_node.chainman);
    auto& evodb = *Assert(m_node.evodb);
    auto wallet = LoadTestWallet(m_node);
    // The notifications are delivered by hand below, so that the planted error meets the wallet on this thread.
    SyncWithValidationInterfaceQueue();
    wallet->m_chain_notifications_handler.reset();
    AddKey(*wallet, coinbaseKey);

    const auto tip = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    const auto locator = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().GetLocator()); };

    // A block processed to the end: the flush records it.
    const CBlock block_a = CreateAndProcessBlock({}, coinbaseKey);
    SyncWithValidationInterfaceQueue();
    const int height_a = tip()->nHeight;
    BOOST_REQUIRE_EQUAL(tip()->GetBlockHash(), block_a.GetHash());
    wallet->blockConnected(block_a, height_a);
    wallet->chainStateFlushed(locator());
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetWalletTx(block_a.vtx[0]->GetHash()) != nullptr);
    }

    // The next block's list diff is missing when the wallet reads the history: the processing fails before the
    // coinbase is written, and the flush that follows must not record this block.
    const CBlock block_b = CreateAndProcessBlock({}, coinbaseKey);
    SyncWithValidationInterfaceQueue();
    const int height_b = tip()->nHeight;
    BOOST_REQUIRE_EQUAL(tip()->GetBlockHash(), block_b.GetHash());
    BOOST_REQUIRE(evodb.CommitRootTransaction());
    const auto diff_key = std::make_pair(CDeterministicMNManager::ListDiffDbKey(), block_b.GetHash());
    CDeterministicMNListDiff diff_b;
    BOOST_REQUIRE(evodb.GetRawDB().Read(diff_key, diff_b));
    BOOST_REQUIRE(evodb.GetRawDB().Erase(diff_key));
    auto fresh_dmnman = std::make_unique<CDeterministicMNManager>(chainman.ActiveChainstate(), evodb);
    std::swap(m_node.dmnman, fresh_dmnman); // the node's own manager, with its caches, waits in fresh_dmnman
    BOOST_CHECK_THROW(wallet->blockConnected(block_b, height_b), dbwrapper_error);
    wallet->chainStateFlushed(locator());
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());

    // The history repaired and the node's manager back, the wallet is loaded again as a start loads it: the
    // recorded best block is the earlier one, so the rescan covers the block that failed, and its coinbase is found.
    BOOST_REQUIRE(evodb.GetRawDB().Write(diff_key, diff_b));
    std::swap(m_node.dmnman, fresh_dmnman);
    fresh_dmnman.reset();
    UnloadTestWallet(std::move(wallet));
    wallet = LoadTestWallet(m_node);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetWalletTx(block_a.vtx[0]->GetHash()) != nullptr);
        BOOST_CHECK(wallet->GetWalletTx(block_b.vtx[0]->GetHash()) != nullptr);
        BOOST_CHECK_EQUAL(wallet->GetLastBlockHash(), block_b.GetHash());
    }
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_b.GetHash());
    UnloadTestWallet(std::move(wallet));
}

BOOST_AUTO_TEST_SUITE_END()
