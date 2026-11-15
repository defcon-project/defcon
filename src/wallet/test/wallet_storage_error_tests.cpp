// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <core_io.h>
#include <dbwrapper.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <interfaces/chain.h>
#include <key.h>
#include <merkleblock.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>
#include <wallet/db.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
// The transactions whose record the wallet database refuses to write, as a full or failing disk refuses a write.
class TxWriteFault
{
    Mutex m_mutex;
    std::set<uint256> m_refused GUARDED_BY(m_mutex);
    int m_refusals GUARDED_BY(m_mutex){0};

public:
    void Refuse(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_refused.insert(hash);
    }
    void Accept(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_refused.erase(hash);
    }
    int Refusals() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return WITH_LOCK(m_mutex, return m_refusals); }
    //! Whether `key` is the record of a refused transaction, as WalletBatch::WriteTx names it; counts the refusal.
    bool Refuses(const CDataStream& key) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        for (const uint256& hash : m_refused) {
            CDataStream record(SER_DISK, CLIENT_VERSION);
            record << std::make_pair(DBKeys::TX, hash);
            if (record.size() == key.size() && std::equal(record.begin(), record.end(), key.begin())) {
                ++m_refusals;
                return true;
            }
        }
        return false;
    }
};

// A key or a value that is serialized already: written and read as it is.
struct RawRecord {
    std::vector<std::byte> bytes;

    RawRecord() = default;
    explicit RawRecord(const CDataStream& stream) : bytes(stream.begin(), stream.end()) {}
    template <typename Stream> void Serialize(Stream& s) const { s.write(Span<const std::byte>{bytes.data(), bytes.size()}); }
    template <typename Stream> void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        s.read(Span<std::byte>{bytes.data(), bytes.size()});
    }
};

// The real database's batch, except that it refuses to write the records TxWriteFault names.
class FaultyBatch : public DatabaseBatch
{
    const std::unique_ptr<DatabaseBatch> m_real;
    const std::shared_ptr<TxWriteFault> m_fault;

    bool ReadKey(CDataStream&& key, CDataStream& value) override
    {
        RawRecord found;
        if (!m_real->Read(RawRecord{key}, found)) return false;
        value.clear();
        value.write(Span<const std::byte>{found.bytes.data(), found.bytes.size()});
        return true;
    }
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override
    {
        if (m_fault->Refuses(key)) return false;
        return m_real->Write(RawRecord{key}, RawRecord{value}, overwrite);
    }
    bool EraseKey(CDataStream&& key) override { return m_real->Erase(RawRecord{key}); }
    bool HasKey(CDataStream&& key) override { return m_real->Exists(RawRecord{key}); }

public:
    FaultyBatch(std::unique_ptr<DatabaseBatch> real, std::shared_ptr<TxWriteFault> fault) : m_real{std::move(real)}, m_fault{std::move(fault)} {}

    void Flush() override { m_real->Flush(); }
    void Close() override { m_real->Close(); }
    bool StartCursor() override { return m_real->StartCursor(); }
    bool ReadAtCursor(CDataStream& key, CDataStream& value, bool& complete) override { return m_real->ReadAtCursor(key, value, complete); }
    void CloseCursor() override { m_real->CloseCursor(); }
    bool TxnBegin() override { return m_real->TxnBegin(); }
    bool TxnCommit() override { return m_real->TxnCommit(); }
    bool TxnAbort() override { return m_real->TxnAbort(); }
};

// The real wallet database behind batches that can refuse a write.
class FaultyDatabase : public WalletDatabase
{
    const std::unique_ptr<WalletDatabase> m_real;
    const std::shared_ptr<TxWriteFault> m_fault;

public:
    FaultyDatabase(std::unique_ptr<WalletDatabase> real, std::shared_ptr<TxWriteFault> fault) : m_real{std::move(real)}, m_fault{std::move(fault)} {}

    void Open() override { m_real->Open(); }
    void AddRef() override { m_real->AddRef(); }
    void RemoveRef() override { m_real->RemoveRef(); }
    bool Rewrite(const char* skip = nullptr) override { return m_real->Rewrite(skip); }
    bool Backup(const std::string& dest) const override { return m_real->Backup(dest); }
    void Flush() override { m_real->Flush(); }
    void Close() override { m_real->Close(); }
    bool PeriodicFlush() override { return m_real->PeriodicFlush(); }
    void IncrementUpdateCounter() override
    {
        ++nUpdateCounter;
        m_real->IncrementUpdateCounter();
    }
    void ReloadDbEnv() override { m_real->ReloadDbEnv(); }
    std::string Filename() override { return m_real->Filename(); }
    std::string Format() override { return m_real->Format(); }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override
    {
        return std::make_unique<FaultyBatch>(m_real->MakeBatch(flush_on_close), m_fault);
    }
    bool SupportsAutoBackup() override { return m_real->SupportsAutoBackup(); }
};

// DIP0003 active from the next block on (regtest activates it at 432): every block connected from here on stores a
// masternode list diff, which the wallet's own block processing reads (AddToWallet -> LockProTxCoins ->
// listMNCollaterials -> GetListForBlock(tip)). As the EvoDB tests' TestChainDIP3Setup: the fixture checks its
// chain against a regtest checkpoint, which exists for 430 blocks, so the 431st is mined here.
struct WalletDIP3ChainSetup : public TestChainSetup {
    WalletDIP3ChainSetup() : TestChainSetup(430) { CreateAndProcessBlock({}, coinbaseKey); }
};

// As wallet_tests.cpp loads and unloads a wallet on the test data directory: the wallet file persists between the
// two, so a second load reads what the first recorded, as a restart does. With `fault`, the wallet's database
// refuses the writes that it names.
std::shared_ptr<CWallet> LoadTestWallet(NodeContext& node, std::shared_ptr<TxWriteFault> fault = nullptr)
{
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto database = MakeWalletDatabase("", options, status, error);
    BOOST_REQUIRE_MESSAGE(database, error.original);
    if (fault) database = std::make_unique<FaultyDatabase>(std::move(database), std::move(fault));
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

// Whether the wallet's database holds a record of the transaction, and whether that record says "conflicted": what
// a start would load, whatever the wallet holds in memory.
bool Stored(CWallet& wallet, const uint256& hash)
{
    return wallet.GetDatabase().MakeBatch()->Exists(std::make_pair(DBKeys::TX, hash));
}

bool StoredAsConflicted(CWallet& wallet, const uint256& hash)
{
    CWalletTx stored(nullptr /* wallet */, nullptr /* tx */);
    BOOST_REQUIRE(wallet.GetDatabase().MakeBatch()->Read(std::make_pair(DBKeys::TX, hash), stored));
    return stored.isConflicted();
}

const CBlockIndex* Tip(ChainstateManager& chainman) { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); }
CBlockLocator Locator(ChainstateManager& chainman) { return WITH_LOCK(::cs_main, return chainman.ActiveChain().GetLocator()); }

// A signed transaction that spends the first output of `from` (paid to `key`) to `pay_to`.
CMutableTransaction SpendFirstOutput(const CTransactionRef& from, const CKey& key, const CScript& pay_to)
{
    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint(from->GetHash(), 0));
    spend.vout.emplace_back(from->vout[0].nValue, pay_to);
    FillableSigningProvider keystore;
    BOOST_REQUIRE(keystore.AddKey(key));
    BOOST_REQUIRE(SignSignature(keystore, *from, spend, 0, SIGHASH_ALL));
    return spend;
}

// A wallet with a faulty database that holds an unconfirmed parent and its child, and a block that confirms another
// spend of the parent's input: both become conflicted, and the database refuses the new record of one of them.
struct RefusedConflict {
    std::shared_ptr<TxWriteFault> fault{std::make_shared<TxWriteFault>()};
    std::shared_ptr<CWallet> wallet;
    uint256 parent;
    uint256 child;
    uint256 refused;
    CBlock block_a; //!< the last block before the conflict: the best block the wallet has recorded
    CBlock block_b; //!< the block with the conflicting spend
    int height_a{0};

    RefusedConflict(WalletDIP3ChainSetup& setup, bool refuse_child)
    {
        auto& chainman = *Assert(setup.m_node.chainman);
        wallet = LoadTestWallet(setup.m_node, fault);
        // The notifications are delivered by hand, one block at a time.
        SyncWithValidationInterfaceQueue();
        wallet->m_chain_notifications_handler.reset();
        AddKey(*wallet, setup.coinbaseKey);

        block_a = setup.CreateAndProcessBlock({}, setup.coinbaseKey);
        SyncWithValidationInterfaceQueue();
        height_a = Tip(chainman)->nHeight;
        wallet->blockConnected(block_a, height_a);
        wallet->chainStateFlushed(Locator(chainman));
        BOOST_REQUIRE_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());

        // The wallet's spend of a mature coinbase and a spend of that, both unconfirmed, and the spend of the same
        // coinbase that a block confirms.
        CKey other;
        other.MakeNewKey(true);
        const CScript to_other{GetScriptForDestination(PKHash(other.GetPubKey()))};
        const CTransactionRef parent_tx = MakeTransactionRef(SpendFirstOutput(setup.m_coinbase_txns[0], setup.coinbaseKey, to_other));
        const CTransactionRef child_tx = MakeTransactionRef(SpendFirstOutput(parent_tx, other, to_other));
        const CMutableTransaction confirmed = SpendFirstOutput(setup.m_coinbase_txns[0], setup.coinbaseKey, GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey()));
        parent = parent_tx->GetHash();
        child = child_tx->GetHash();
        refused = refuse_child ? child : parent;
        BOOST_REQUIRE(parent != confirmed.GetHash());
        BOOST_REQUIRE(wallet->AddToWallet(parent_tx, {}) != nullptr);
        BOOST_REQUIRE(wallet->AddToWallet(child_tx, {}) != nullptr);

        block_b = setup.CreateAndProcessBlock({confirmed}, setup.coinbaseKey);
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_EQUAL(Tip(chainman)->GetBlockHash(), block_b.GetHash());
        fault->Refuse(refused);
        wallet->blockConnected(block_b, Tip(chainman)->nHeight);
        BOOST_CHECK_EQUAL(fault->Refusals(), 1);
        // In memory both are conflicted; in the database only the one whose record was written.
        BOOST_CHECK(Conflicted(parent));
        BOOST_CHECK(Conflicted(child));
        BOOST_CHECK_EQUAL(StoredAsConflicted(*wallet, parent), refused != parent);
        BOOST_CHECK_EQUAL(StoredAsConflicted(*wallet, child), refused != child);
        // The flush that follows must not record the block.
        wallet->chainStateFlushed(Locator(chainman));
        BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());
    }

    bool Conflicted(const uint256& hash)
    {
        LOCK(wallet->cs_wallet);
        const CWalletTx* wtx = wallet->GetWalletTx(hash);
        BOOST_REQUIRE(wtx != nullptr);
        return wtx->isConflicted();
    }

    // The wallet loaded again as a start loads it, on a database that writes.
    void Reload(NodeContext& node)
    {
        UnloadTestWallet(std::move(wallet));
        wallet = LoadTestWallet(node);
    }
};
} // namespace

RPCHelpMan importprunedfunds();

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

// A transaction the wallet could not save must be found again by the next start. When the database refuses a
// transaction's record, the block notification still returns in order, with the transaction in memory only. Had a
// later flush recorded the tip, the next start would have had nothing to rescan, and the transaction would have
// been missing from the wallet from then on.
BOOST_AUTO_TEST_CASE(a_transaction_the_wallet_could_not_save_is_rescanned_on_the_next_start)
{
    auto& chainman = *Assert(m_node.chainman);
    const auto fault = std::make_shared<TxWriteFault>();
    auto wallet = LoadTestWallet(m_node, fault);
    // The notifications are delivered by hand below, one block at a time.
    SyncWithValidationInterfaceQueue();
    wallet->m_chain_notifications_handler.reset();
    AddKey(*wallet, coinbaseKey);

    const auto tip = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    const auto locator = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().GetLocator()); };

    // A block whose coinbase is saved: the flush records it.
    const CBlock block_a = CreateAndProcessBlock({}, coinbaseKey);
    SyncWithValidationInterfaceQueue();
    wallet->blockConnected(block_a, tip()->nHeight);
    wallet->chainStateFlushed(locator());
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());

    // The database refuses the coinbase of the next block. The notification returns, the wallet holds the
    // transaction in memory, and the flush that follows must not record this block.
    const CBlock block_b = CreateAndProcessBlock({}, coinbaseKey);
    SyncWithValidationInterfaceQueue();
    fault->Refuse(block_b.vtx[0]->GetHash());
    wallet->blockConnected(block_b, tip()->nHeight);
    BOOST_CHECK_EQUAL(fault->Refusals(), 1);
    BOOST_CHECK(!Stored(*wallet, block_b.vtx[0]->GetHash()));
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetWalletTx(block_b.vtx[0]->GetHash()) != nullptr);
    }
    wallet->chainStateFlushed(locator());
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());

    // Nor may a later block move the record, although its own coinbase is saved: the record stays before the
    // transaction that is not on disk.
    const CBlock block_c = CreateAndProcessBlock({}, coinbaseKey);
    SyncWithValidationInterfaceQueue();
    wallet->blockConnected(block_c, tip()->nHeight);
    BOOST_CHECK_EQUAL(fault->Refusals(), 1);
    wallet->chainStateFlushed(locator());
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_a.GetHash());

    // The wallet is loaded again as a start loads it, on a database that writes: the rescan from the recorded
    // block finds the coinbase that was not saved.
    UnloadTestWallet(std::move(wallet));
    wallet = LoadTestWallet(m_node);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetWalletTx(block_a.vtx[0]->GetHash()) != nullptr);
        BOOST_CHECK(wallet->GetWalletTx(block_b.vtx[0]->GetHash()) != nullptr);
        BOOST_CHECK(wallet->GetWalletTx(block_c.vtx[0]->GetHash()) != nullptr);
        BOOST_CHECK_EQUAL(wallet->GetLastBlockHash(), block_c.GetHash());
    }
    BOOST_CHECK_EQUAL(RecordedBestBlock(*wallet), block_c.GetHash());
    UnloadTestWallet(std::move(wallet));
}

// The same for a conflict. A block spends a coin that an unconfirmed transaction of the wallet spends too, and the
// database refuses the record of that transaction, now conflicted. Had the flush recorded the block, the next start
// would have loaded the transaction as unconfirmed, with nothing left to tell it that it can never confirm. Before
// that start, a rescan over the block must not report a success the database does not hold: it writes the record
// again, and fails for as long as that write fails.
BOOST_AUTO_TEST_CASE(a_conflict_the_wallet_could_not_save_is_written_by_a_later_rescan)
{
    auto& chainman = *Assert(m_node.chainman);
    RefusedConflict conflict(*this, /* refuse_child= */ false);
    CWallet& wallet = *conflict.wallet;
    {
        WalletRescanReserver reserver(wallet);
        BOOST_REQUIRE(reserver.reserve());
        const auto rescan = [&] {
            return wallet.ScanForWalletTransactions(conflict.block_a.GetHash(), conflict.height_a, {}, reserver, /* fUpdate= */ true);
        };
        // The database still refuses the record.
        CWallet::ScanResult result = rescan();
        BOOST_CHECK_EQUAL(conflict.fault->Refusals(), 2);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, conflict.block_b.GetHash());
        BOOST_CHECK(!StoredAsConflicted(wallet, conflict.parent));
        // It writes again.
        conflict.fault->Accept(conflict.parent);
        result = rescan();
        BOOST_CHECK_EQUAL(conflict.fault->Refusals(), 2);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK(StoredAsConflicted(wallet, conflict.parent));
        BOOST_CHECK(StoredAsConflicted(wallet, conflict.child));
    }
    // The recorded best block stays where it is until the next start all the same.
    wallet.chainStateFlushed(Locator(chainman));
    BOOST_CHECK_EQUAL(RecordedBestBlock(wallet), conflict.block_a.GetHash());

    conflict.Reload(m_node);
    BOOST_CHECK(conflict.Conflicted(conflict.parent));
    BOOST_CHECK(conflict.Conflicted(conflict.child));
    BOOST_CHECK_EQUAL(RecordedBestBlock(*conflict.wallet), conflict.block_b.GetHash());
    UnloadTestWallet(std::move(conflict.wallet));
}

// A conflict saved for the parent only. The record of the parent is written, that of its child is refused, and the
// node restarts. The start loads a conflicted parent and an unconfirmed child, and its rescan meets the block again:
// the parent has that conflict already, and the conflict must still reach the child behind it.
BOOST_AUTO_TEST_CASE(a_conflict_saved_for_the_parent_only_reaches_the_child_on_the_next_start)
{
    RefusedConflict conflict(*this, /* refuse_child= */ true);

    conflict.Reload(m_node);
    BOOST_CHECK(conflict.Conflicted(conflict.parent));
    BOOST_CHECK(conflict.Conflicted(conflict.child));
    BOOST_CHECK(StoredAsConflicted(*conflict.wallet, conflict.parent));
    BOOST_CHECK(StoredAsConflicted(*conflict.wallet, conflict.child));
    BOOST_CHECK_EQUAL(RecordedBestBlock(*conflict.wallet), conflict.block_b.GetHash());
    UnloadTestWallet(std::move(conflict.wallet));
}

// A rescan that could not save a transaction it found reports the block as failed, as it reports a block it could
// not read: the rescan of the next start does not reach back to an old block, so the caller has to know. And it
// goes on reporting it for as long as the write fails, although the wallet holds the transaction in memory by then.
BOOST_AUTO_TEST_CASE(a_rescan_reports_the_block_until_the_transaction_is_saved)
{
    auto& chainman = *Assert(m_node.chainman);
    const auto fault = std::make_shared<TxWriteFault>();
    auto wallet = LoadTestWallet(m_node, fault);
    SyncWithValidationInterfaceQueue();
    wallet->m_chain_notifications_handler.reset();
    // The key is new to the wallet, so a rescan finds the coinbase of every block it reads. The last blocks of
    // the chain are enough: the scan starts 30 blocks below the tip, and the refused coinbase is 20 below it.
    AddKey(*wallet, coinbaseKey);

    const CBlockIndex* const tip = Tip(chainman);
    const CBlockIndex* const first_block = WITH_LOCK(::cs_main, return chainman.ActiveChain()[tip->nHeight - 30]);
    const CBlockIndex* const refused_block = WITH_LOCK(::cs_main, return chainman.ActiveChain()[tip->nHeight - 20]);
    CBlock block;
    BOOST_REQUIRE(ReadBlockFromDisk(block, refused_block, Params().GetConsensus()));
    const uint256 refused{block.vtx[0]->GetHash()};
    fault->Refuse(refused);
    // The wallet is new: the best block it has recorded is the tip, so its next start rescans nothing.
    BOOST_REQUIRE_EQUAL(RecordedBestBlock(*wallet), tip->GetBlockHash());
    {
        // The reservation refers to the wallet, so it ends before the wallet is unloaded.
        WalletRescanReserver reserver(*wallet);
        BOOST_REQUIRE(reserver.reserve());
        const auto rescan = [&] {
            return wallet->ScanForWalletTransactions(first_block->GetBlockHash(), first_block->nHeight, {}, reserver, /* fUpdate= */ false);
        };
        CWallet::ScanResult result = rescan();
        BOOST_CHECK_EQUAL(fault->Refusals(), 1);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, refused_block->GetBlockHash());
        // The scan went on to the tip, as it does past a block it cannot read.
        BOOST_CHECK_EQUAL(result.last_scanned_block, tip->GetBlockHash());
        BOOST_CHECK(!Stored(*wallet, refused));

        // The same rescan again, the write still refused: the transaction is in the wallet's memory now, and
        // the block is reported all the same.
        result = rescan();
        BOOST_CHECK_EQUAL(fault->Refusals(), 2);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, refused_block->GetBlockHash());
        BOOST_CHECK(!Stored(*wallet, refused));

        // Once the database writes, the rescan saves the transaction and succeeds.
        fault->Accept(refused);
        result = rescan();
        BOOST_CHECK_EQUAL(fault->Refusals(), 2);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK(Stored(*wallet, refused));
    }

    // Loaded again, with nothing to rescan: the transaction is there because its record is.
    UnloadTestWallet(std::move(wallet));
    wallet = LoadTestWallet(m_node);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetWalletTx(refused) != nullptr);
    }
    UnloadTestWallet(std::move(wallet));
}

// importprunedfunds adds one transaction of an old block, which no later start rescans. It fails when the
// transaction could not be saved, and goes on failing when it is called again while the write still fails.
BOOST_AUTO_TEST_CASE(importprunedfunds_fails_until_the_transaction_is_saved)
{
    auto& chainman = *Assert(m_node.chainman);
    const auto fault = std::make_shared<TxWriteFault>();
    auto wallet = LoadTestWallet(m_node, fault);
    SyncWithValidationInterfaceQueue();
    wallet->m_chain_notifications_handler.reset();
    AddKey(*wallet, coinbaseKey);

    // The coinbase of a block 20 below the tip, with the proof that the block holds it.
    const CBlockIndex* const tip = Tip(chainman);
    const CBlockIndex* const old_block = WITH_LOCK(::cs_main, return chainman.ActiveChain()[tip->nHeight - 20]);
    CBlock block;
    BOOST_REQUIRE(ReadBlockFromDisk(block, old_block, Params().GetConsensus()));
    const uint256 imported{block.vtx[0]->GetHash()};
    CDataStream proof(SER_NETWORK, PROTOCOL_VERSION);
    proof << CMerkleBlock(block, std::set<uint256>{imported});
    JSONRPCRequest request;
    request.params.setArray();
    request.params.push_back(EncodeHexTx(*block.vtx[0]));
    request.params.push_back(HexStr(proof));
    BOOST_REQUIRE_EQUAL(RecordedBestBlock(*wallet), tip->GetBlockHash());

    fault->Refuse(imported);
    BOOST_CHECK_THROW(importprunedfunds().HandleRequest(request), UniValue);
    BOOST_CHECK_EQUAL(fault->Refusals(), 1);
    BOOST_CHECK(!Stored(*wallet, imported));
    // Again, the write still refused: the transaction is in the wallet's memory now.
    BOOST_CHECK_THROW(importprunedfunds().HandleRequest(request), UniValue);
    BOOST_CHECK_EQUAL(fault->Refusals(), 2);
    BOOST_CHECK(!Stored(*wallet, imported));
    // Once the database writes, the call saves the transaction and succeeds.
    fault->Accept(imported);
    BOOST_CHECK_NO_THROW(importprunedfunds().HandleRequest(request));
    BOOST_CHECK_EQUAL(fault->Refusals(), 2);
    BOOST_CHECK(Stored(*wallet, imported));

    // Loaded again, with nothing to rescan: the transaction is there because its record is.
    UnloadTestWallet(std::move(wallet));
    wallet = LoadTestWallet(m_node);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetWalletTx(imported) != nullptr);
    }
    UnloadTestWallet(std::move(wallet));
}

BOOST_AUTO_TEST_SUITE_END()
