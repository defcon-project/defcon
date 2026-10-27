// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <dbwrapper.h>
#include <evo/evodb.h>
#include <node/coinstats.h>
#include <script/script.h>
#include <sync.h>
#include <txdb.h>
#include <test/util/setup_common.h>
#include <test/util/logging.h>
#include <leveldb/env.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/string.h>
#include <validation.h>

#include <cstdarg>
#include <cwchar>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>

#include <boost/test/unit_test.hpp>

// Test if a string consists entirely of null characters
static bool is_null_key(const std::vector<unsigned char>& key) {
    bool isnull = true;

    for (unsigned int i = 0; i < key.size(); i++)
        isnull &= (key[i] == '\x00');

    return isnull;
}


namespace {
class CopyFaultValue
{
public:
    int m_value{0};
    static int s_copies_before_throw;

    CopyFaultValue() = default;
    explicit CopyFaultValue(int value) : m_value{value} {}
    CopyFaultValue(const CopyFaultValue& other) : m_value{other.m_value}
    {
        if (s_copies_before_throw == 0) throw std::runtime_error("injected value copy failure");
        if (s_copies_before_throw > 0) --s_copies_before_throw;
    }
    CopyFaultValue& operator=(const CopyFaultValue& other)
    {
        m_value = other.m_value;
        return *this;
    }

    SERIALIZE_METHODS(CopyFaultValue, obj) { READWRITE(obj.m_value); }
};

int CopyFaultValue::s_copies_before_throw{-1};

struct SerializeFault
{
    template <typename Stream> void Serialize(Stream& stream) const
    {
        stream << uint8_t{'x'};
        throw std::runtime_error("injected serialization failure");
    }
};

// One valid record followed by an injected storage failure, or healthy EOF.
class FaultIterator : public leveldb::Iterator
{
    const leveldb::Status m_failure;
    bool m_positioned{false};
    bool m_at_end{false};
    const std::string m_key{"a"};
    const std::string m_value{"v"};
public:
    explicit FaultIterator(leveldb::Status failure) : m_failure(std::move(failure)) {}
    bool Valid() const override { return m_positioned && !m_at_end; }
    void SeekToFirst() override { m_positioned = true; m_at_end = false; }
    void SeekToLast() override { SeekToFirst(); }
    void Seek(const leveldb::Slice&) override { SeekToFirst(); }
    void Next() override { m_at_end = true; }
    void Prev() override { m_at_end = true; }
    leveldb::Slice key() const override { return m_key; }
    leveldb::Slice value() const override { return m_value; }
    leveldb::Status status() const override { return m_at_end ? m_failure : leveldb::Status::OK(); }
};

class LevelDBLogCapture
{
    const bool m_was_enabled{LogInstance().WillLogCategory(BCLog::LEVELDB)};
    const BCLog::Level m_level{LogInstance().LogLevel()};
    std::list<std::function<void(const std::string&)>>::iterator m_connection;
public:
    std::vector<std::string> lines;
    LevelDBLogCapture()
    {
        LogInstance().EnableCategory(BCLog::LEVELDB);
        LogInstance().SetLogLevel(BCLog::Level::Debug);
        m_connection = LogInstance().PushBackCallback([this](const std::string& line) { lines.push_back(line); });
    }
    ~LevelDBLogCapture()
    {
        LogInstance().DeleteCallback(m_connection);
        LogInstance().SetLogLevel(m_level);
        if (!m_was_enabled) LogInstance().DisableCategory(BCLog::LEVELDB);
    }
};

void LogLevelDB(leveldb::Logger& logger, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    logger.Logv(format, args);
    va_end(args);
}

Coin TestCoin()
{
    return Coin{CTxOut{1 * COIN, CScript() << OP_TRUE}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/false};
}

//! A coin record key as CCoinsViewDB writes it, built by hand so a test can damage it.
CDataStream CoinKey(const uint256& hash, std::optional<uint32_t> n, bool trailing_byte = false)
{
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << uint8_t{'C'} << hash;
    if (n) {
        uint32_t index{*n};
        key << VARINT(index);
    }
    if (trailing_byte) key << uint8_t{0};
    return key;
}

//! Write healthy coins through CCoinsViewDB, then add raw coin records with valid
//! values, a head-blocks marker and a legacy per-tx record after the coin range.
void WriteCoinsDB(const fs::path& path, const std::vector<COutPoint>& coins, const uint256& best,
                  const std::vector<CDataStream>& raw_coin_keys)
{
    {
        CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/true);
        CCoinsMapMemoryResource resource;
        CCoinsMap map{0, CCoinsMap::hasher{}, CCoinsMap::key_equal{}, &resource};
        for (const auto& outpoint : coins) {
            map.emplace(outpoint, CCoinsCacheEntry{TestCoin(), CCoinsCacheEntry::DIRTY});
        }
        BOOST_REQUIRE(view.BatchWrite(map, best));
    }
    CDBWrapper raw(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false, /*obfuscate=*/true);
    CDBBatch batch(raw);
    for (const auto& key : raw_coin_keys) {
        batch.Write(key, TestCoin());
    }
    batch.Write(uint8_t{'H'}, std::vector<uint256>{best, uint256()});
    batch.Write(std::make_pair(uint8_t{'c'}, uint256{0x10}), uint8_t{0});
    BOOST_REQUIRE(raw.WriteBatch(batch));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(dbwrapper_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(dbwrapper_batch_exception_reuse)
{
    for (const bool obfuscate : {false, true}) {
        CDBWrapper db(m_args.GetDataDirBase() / "batch_fault", 1 << 20, true, false, obfuscate);
        for (const bool clear : {false, true}) {
            for (const int fault : {0, 1, 2, 3}) {
                CDBBatch batch(db);
                batch.Write(uint8_t{'a'}, uint32_t{1});
                const auto before = batch.SizeEstimate();
                if (fault == 0) BOOST_CHECK_THROW(batch.Write(SerializeFault{}, uint32_t{2}), std::runtime_error);
                if (fault == 1) BOOST_CHECK_THROW(batch.Write(uint8_t{'b'}, SerializeFault{}), std::runtime_error);
                if (fault == 2) BOOST_CHECK_THROW(batch.Erase(SerializeFault{}), std::runtime_error);
                if (fault == 3) {
                    CDataStream key(SER_DISK, CLIENT_VERSION);
                    key << uint8_t{'b'};
                    BOOST_CHECK_THROW(batch.Write(key, SerializeFault{}), std::runtime_error);
                }
                BOOST_CHECK_EQUAL(batch.SizeEstimate(), before);
                if (clear) {
                    batch.Clear();
                    BOOST_CHECK_EQUAL(batch.SizeEstimate(), 0U);
                }
                batch.Write(uint8_t{'b'}, uint32_t{2});
                batch.Erase(uint8_t{'c'});
                CDBBatch fresh(db);
                if (!clear) fresh.Write(uint8_t{'a'}, uint32_t{1});
                fresh.Write(uint8_t{'b'}, uint32_t{2});
                fresh.Erase(uint8_t{'c'});
                BOOST_CHECK_EQUAL(batch.SizeEstimate(), fresh.SizeEstimate());
                BOOST_REQUIRE(db.WriteBatch(batch));
                uint32_t value{0};
                BOOST_REQUIRE(db.Read(uint8_t{'b'}, value));
                BOOST_CHECK_EQUAL(value, 2U);
                if (!clear) {
                    BOOST_REQUIRE(db.Read(uint8_t{'a'}, value));
                    BOOST_CHECK_EQUAL(value, 1U);
                }
                BOOST_CHECK(!db.Exists(uint8_t{'c'}));
                db.Erase(uint8_t{'a'});
                db.Erase(uint8_t{'b'});
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_iterator_storage_errors)
{
    CDBWrapper db(m_args.GetDataDirBase() / "iterator_fault", 1 << 20, true);
    for (const auto& error : {leveldb::Status::IOError("injected iterator I/O failure"),
                              leveldb::Status::Corruption("injected iterator corruption")}) {
        CDBIterator first(db, leveldb::NewErrorIterator(error));
        BOOST_CHECK_THROW(first.Valid(), dbwrapper_error);
        BOOST_CHECK_THROW(first.SeekToFirst(), dbwrapper_error);
        BOOST_CHECK_THROW(first.Seek(uint8_t{'a'}), dbwrapper_error);
        for (const bool seek : {false, true}) {
            CDBIterator mid(db, new FaultIterator(error));
            if (seek) mid.Seek(uint8_t{'a'}); else mid.SeekToFirst();
            BOOST_REQUIRE(mid.Valid());
            uint8_t key{0};
            BOOST_REQUIRE(mid.GetKey(key));
            BOOST_CHECK_EQUAL(key, uint8_t{'a'});
            BOOST_CHECK_THROW(mid.Next(), dbwrapper_error);
            BOOST_CHECK_THROW(mid.Valid(), dbwrapper_error);
        }
        struct Parent {
            CDBWrapper& db;
            const leveldb::Status& error;
            CDBIterator* NewIterator() { return new CDBIterator(db, leveldb::NewErrorIterator(error)); }
        } parent{db, error};
        CDBBatch batch(db);
        CDBTransaction<Parent, CDBBatch> tx(parent, batch);
        tx.Write(uint8_t{'z'}, uint32_t{1});
        auto overlay = tx.NewIteratorUniquePtr();
        BOOST_CHECK_THROW(overlay->SeekToFirst(), dbwrapper_error);
        BOOST_CHECK_THROW(overlay->Valid(), dbwrapper_error);
    }
    CDBIterator empty(db, leveldb::NewEmptyIterator());
    empty.SeekToFirst();
    BOOST_CHECK(!empty.Valid());
    CDBIterator healthy(db, new FaultIterator(leveldb::Status::OK()));
    healthy.SeekToFirst();
    BOOST_REQUIRE(healthy.Valid());
    healthy.Next();
    BOOST_CHECK(!healthy.Valid());
    BOOST_CHECK(db.IsEmpty());
    BOOST_REQUIRE(db.Write(uint8_t{'a'}, uint32_t{1}));
    BOOST_CHECK(!db.IsEmpty());
}

BOOST_AUTO_TEST_CASE(dbwrapper_iterator_corrupt_table)
{
    const auto path = m_args.GetDataDirBase() / "corrupt_iterator";
    {
        CDBWrapper db(path, 1 << 20);
        BOOST_REQUIRE(db.Write(uint8_t{'a'}, uint32_t{1}));
        db.CompactFull();
    }
    size_t tables{0};
    for (const auto& entry : fs::directory_iterator(path)) {
        const auto extension = entry.path().extension();
        if (extension == ".ldb" || extension == ".sst") {
            std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
            BOOST_REQUIRE(file.good());
            file.put('x');
            ++tables;
        }
    }
    BOOST_REQUIRE(tables > 0);
    {
        CDBWrapper db(path, 1 << 20);
        BOOST_CHECK_THROW(db.IsEmpty(), dbwrapper_error);
        std::unique_ptr<CDBIterator> it(db.NewIterator());
        BOOST_CHECK_THROW(it->SeekToFirst(), dbwrapper_error);
    }
    // A failing emptiness check must neither install a new obfuscation key nor retain the LOCK.
    BOOST_CHECK_THROW(CDBWrapper(path, 1 << 20, false, false, true), dbwrapper_error);
    CDBWrapper reopened(path, 1 << 20);
    BOOST_CHECK(is_null_key(dbwrapper_private::GetObfuscateKey(reopened)));
    BOOST_CHECK_THROW(reopened.IsEmpty(), dbwrapper_error);
}

BOOST_AUTO_TEST_CASE(dbwrapper_constructor_rollback)
{
    for (const bool memory : {false, true}) {
        const auto path = m_args.GetDataDirBase() / (memory ? "ctor_memory" : "ctor_disk");
        if (!memory) {
            CDBWrapper seed(path, 1 << 20);
            BOOST_REQUIRE(seed.Write(uint8_t{'a'}, uint32_t{7}));
        }
        {
            DebugLogHelper fail_after_open{"Opened LevelDB successfully", [](const std::string* line) {
                if (line) throw std::runtime_error("injected post-open constructor failure");
                return false;
            }};
            BOOST_CHECK_EXCEPTION(CDBWrapper(path, 1 << 20, memory), std::runtime_error,
                [](const auto& e) { return std::string{e.what()} == "injected post-open constructor failure"; });
        }
        // A failed constructor must release the disk LOCK and all in-memory resources.
        CDBWrapper reopened(path, 1 << 20, memory);
        if (!memory) {
            uint32_t persisted{0};
            BOOST_REQUIRE(reopened.Read(uint8_t{'a'}, persisted));
            BOOST_CHECK_EQUAL(persisted, 7U);
        }
        BOOST_REQUIRE(reopened.Write(uint8_t{'a'}, uint32_t{42}));
        uint32_t value{0};
        BOOST_REQUIRE(reopened.Read(uint8_t{'a'}, value));
        BOOST_CHECK_EQUAL(value, 42U);
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_logger_boundaries)
{
    auto logger = dbwrapper_private::CreateLevelDBLogger();
    LevelDBLogCapture capture;
    for (const size_t length : {0U, 1U, 498U, 499U, 500U, 501U, 29998U, 29999U, 30000U, 30001U}) {
        for (const bool newline : {false, true}) {
            std::string message(length, 'x');
            if (newline && !message.empty()) message.back() = '\n';
            capture.lines.clear();
            LogLevelDB(*logger, "%s", message.c_str());
            BOOST_REQUIRE_EQUAL(capture.lines.size(), 1U);
            const size_t capacity = length < 500 ? 500 : 30000;
            std::string expected = message.substr(0, capacity - 1);
            if (expected.empty() || expected.back() != '\n') expected += '\n';
            if (expected.size() >= capacity) expected.resize(capacity - 1);
            const auto& actual = capture.lines.front();
            BOOST_REQUIRE(actual.size() >= expected.size());
            BOOST_CHECK_EQUAL(actual.substr(actual.size() - expected.size()), expected);
        }
    }
    capture.lines.clear();
    LogInstance().DisableCategory(BCLog::LEVELDB);
    LogLevelDB(*logger, "%s", "not logged");
    BOOST_CHECK(capture.lines.empty());
}

BOOST_AUTO_TEST_CASE(dbwrapper_logger_format_error)
{
    auto logger = dbwrapper_private::CreateLevelDBLogger();
    LevelDBLogCapture capture;
    // A surrogate cannot be encoded as a multibyte character.
    LogLevelDB(*logger, "%lc", static_cast<wint_t>(0xd800));
    BOOST_CHECK(capture.lines.empty());
}

// Verify that Read() returns false (without throwing) when the stored value
// fails to be deserialized
BOOST_AUTO_TEST_CASE(dbwrapper_read_returns_false_on_deserialization_error)
{
    for (const bool obfuscate : {false, true}) {
        const fs::path path{m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_deser_obf" : "dbwrapper_deser_noobf")};
        CDBWrapper dbw(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/true, obfuscate);

        constexpr uint8_t key{'X'};

        // Write a single byte. uint256 requires 32 bytes, so reading this key
        // as uint256 must trigger a deserialization error inside Read()
        BOOST_REQUIRE(dbw.Write(key, uint8_t{0xFF}));
        BOOST_CHECK(dbw.Exists(key));

        // Read() must catch the deserialization exception and return false,
        // the same as if the key were absent
        uint256 result;
        BOOST_CHECK(!dbw.Read(key, result));
    }
}

// Verify Read() throws dbwrapper_error due to an internal db error
BOOST_AUTO_TEST_CASE(dbwrapper_read_throws_on_db_error)
{
    const fs::path path{m_args.GetDataDirBase() / "dbwrapper_db_error"};
    constexpr uint8_t key{'Y'};

    // Write a value and compact it into a table file rather than leaving it in the log.
    {
        CDBWrapper db(path, 1 << 20);
        BOOST_REQUIRE(db.Write(key, InsecureRand256()));
        db.CompactFull();
    }

    // Corrupt every table so any subsequent Read() fails
    size_t tables{0};
    for (const auto& entry : fs::directory_iterator(path)) {
        const auto extension = entry.path().extension();
        if (extension == ".ldb" || extension == ".sst") {
            std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
            BOOST_REQUIRE(file.good());
            file.put('\xff');
            ++tables;
        }
    }
    BOOST_REQUIRE(tables > 0);

    // Read() should detect the issue now and throw
    const CDBWrapper db(path, 1 << 20);
    uint256 result;
    BOOST_CHECK_EXCEPTION(db.Read(key, result), dbwrapper_error, HasReason("Fatal LevelDB error"));

    // TryRead() must return DatabaseError (without throwing).
    CDBWrapper::ReadStatus status = db.TryRead(key, result);
    BOOST_REQUIRE(!status);
    BOOST_CHECK(status.error().status == CDBWrapper::ReadFailure::Code::DatabaseError);
    BOOST_CHECK(status.error().err_msg.find("Fatal LevelDB error") != std::string::npos);
}

// Exercise TryRead() return values directly: found, absent and DeserializationError.
// DatabaseError is tested inside 'dbwrapper_read_throws_on_db_error' test
BOOST_AUTO_TEST_CASE(dbwrapper_tryread)
{
    for (const bool obfuscate : {false, true}) {
        const fs::path path{m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_tryread_obf" : "dbwrapper_tryread_noobf")};
        CDBWrapper dbw(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/true, obfuscate);

        constexpr uint8_t key_ok{'A'};
        constexpr uint8_t key_missing{'B'};
        constexpr uint8_t key_bad{'C'};

        uint256 written_value{InsecureRand256()};
        BOOST_REQUIRE(dbw.Write(key_ok, written_value));
        BOOST_REQUIRE(dbw.Write(key_bad, uint8_t{0xFF}));

        // Found: key exists, value deserializes correctly
        {
            uint256 read_value;
            CDBWrapper::ReadStatus status = dbw.TryRead(key_ok, read_value);
            BOOST_REQUIRE(status);
            BOOST_CHECK(status.value());
            BOOST_CHECK_EQUAL(read_value, written_value);
        }

        // Absent: key does not exist
        {
            uint256 read_value;
            CDBWrapper::ReadStatus status = dbw.TryRead(key_missing, read_value);
            BOOST_REQUIRE(status);
            BOOST_CHECK(!status.value());
        }

        // DeserializationError: key exists but stored value is too short
        {
            uint256 read_value;
            CDBWrapper::ReadStatus status = dbw.TryRead(key_bad, read_value);
            BOOST_REQUIRE(!status);
            BOOST_CHECK(status.error().status == CDBWrapper::ReadFailure::Code::DeserializationError);
            BOOST_CHECK(!status.error().err_msg.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(coinsviewdb_head_blocks_decode)
{
    const fs::path path{m_args.GetDataDirBase() / "coinsviewdb_head_blocks"};
    const std::vector<uint256> heads{InsecureRand256(), InsecureRand256()};
    CCoinsMapMemoryResource resource;
    CCoinsMap map{0, CCoinsMap::hasher{}, CCoinsMap::key_equal{}, &resource};
    const auto set_marker = [&](const auto& value) {
        CDBWrapper raw(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false, /*obfuscate=*/true);
        BOOST_REQUIRE(raw.Write(uint8_t{'H'}, value));
    };

    // An absent marker means a consistent database.
    {
        CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/true);
        BOOST_CHECK(view.GetHeadBlocks().empty());
    }

    // A valid marker is returned, and the next flush to its new tip completes the transition.
    set_marker(heads);
    {
        CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
        BOOST_CHECK(view.GetHeadBlocks() == heads);
        BOOST_REQUIRE(view.BatchWrite(map, heads[0]));
        BOOST_CHECK(view.GetHeadBlocks().empty());
        BOOST_CHECK_EQUAL(view.GetBestBlock(), heads[0]);
    }

    // A present marker that does not decode, or names no block, is an error rather than
    // an absent marker: a two-entry marker with one hash, a bare two-entry count, and an
    // empty marker.
    for (const int variant : {0, 1, 2}) {
        if (variant == 0) {
            set_marker(std::make_pair(uint8_t{2}, heads[0]));
        } else if (variant == 1) {
            set_marker(uint8_t{2});
        } else {
            set_marker(std::vector<uint256>{});
        }
        {
            CDBWrapper raw(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false, /*obfuscate=*/true);
            BOOST_REQUIRE(raw.Erase(uint8_t{'B'}));
        }
        CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
        BOOST_CHECK_EXCEPTION(view.GetHeadBlocks(), dbwrapper_error,
                              HasReason(variant == 2 ? "Head blocks marker is present but empty" : "Head blocks deserialization failure"));
        // Without a best block, a flush consults the marker and must not replace it.
        BOOST_CHECK_THROW(view.BatchWrite(map, heads[0]), dbwrapper_error);
        BOOST_CHECK_THROW(view.GetHeadBlocks(), dbwrapper_error);
    }
}

BOOST_AUTO_TEST_CASE(coinsviewdb_cursor_key_decode)
{
    const uint256 best{InsecureRand256()};
    const std::vector<COutPoint> healthy{{uint256{0x10}, 0}, {uint256{0x10}, 1}, {uint256{0x20}, 0}};
    const fs::path base{m_args.GetDataDirBase()};

    // Healthy coin keys, followed by records outside the coin range, end the scan normally.
    WriteCoinsDB(base / "cursor_healthy", healthy, best, {});
    {
        CCoinsViewDB view(base / "cursor_healthy", 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
        auto cursor = view.Cursor();
        std::vector<COutPoint> keys;
        while (cursor->Valid()) {
            COutPoint key;
            Coin coin;
            BOOST_REQUIRE(cursor->GetKey(key));
            BOOST_REQUIRE(cursor->GetValue(coin));
            keys.push_back(key);
            cursor->Next();
        }
        BOOST_CHECK(keys == healthy);
    }

    // A damaged first coin key must not be returned as a coin.
    WriteCoinsDB(base / "cursor_first", healthy, best, {CoinKey(uint256{0x01}, std::nullopt)});
    {
        CCoinsViewDB view(base / "cursor_first", 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
        BOOST_CHECK_EXCEPTION(view.Cursor(), dbwrapper_error, HasReason("Coin database key decode failure"));
    }

    // A damaged later coin key, or one with trailing data, must not look like the end of the coins.
    for (const bool trailing : {false, true}) {
        const fs::path path{base / (trailing ? "cursor_trailing" : "cursor_later")};
        WriteCoinsDB(path, healthy, best, {trailing ? CoinKey(uint256{0xf0}, 0, /*trailing_byte=*/true) : CoinKey(uint256{0xf0}, std::nullopt)});
        CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
        auto cursor = view.Cursor();
        size_t rows{0};
        const auto scan = [&] {
            while (cursor->Valid()) {
                ++rows;
                cursor->Next();
            }
        };
        BOOST_CHECK_EXCEPTION(scan(), dbwrapper_error, HasReason(trailing ? "Coin database key has trailing data" : "Coin database key decode failure"));
        BOOST_CHECK_EQUAL(rows, healthy.size());
        BOOST_CHECK(!cursor->Valid());
    }
}

BOOST_FIXTURE_TEST_CASE(coinsviewdb_utxo_stats_bad_key, TestingSetup)
{
    // UTXO statistics must fail instead of reporting the coins before a damaged key.
    const fs::path path{m_args.GetDataDirBase() / "utxo_stats_bad_key"};
    const std::vector<COutPoint> healthy{{uint256{0x10}, 0}, {uint256{0x20}, 0}};
    WriteCoinsDB(path, healthy, InsecureRand256(), {CoinKey(uint256{0xf0}, std::nullopt)});
    CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
    const CBlockIndex* tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
    CCoinsStats stats{CoinStatsHashType::NONE};
    stats.index_requested = false;
    BOOST_CHECK_EXCEPTION(GetUTXOStats(&view, m_node.chainman->m_blockman, stats, [] {}, tip),
                          dbwrapper_error, HasReason("Coin database key decode failure"));
}

BOOST_AUTO_TEST_CASE(coinsviewdb_resize_reopen_failure)
{
    const fs::path path{m_args.GetDataDirBase() / "coinsviewdb_resize"};
    const COutPoint outpoint{InsecureRand256(), 0};
    const uint256 best{InsecureRand256()};
    WriteCoinsDB(path, {outpoint}, best, {});
    CCoinsViewDB view(path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false);
    LOCK(::cs_main);

    // The first `failures` database opens throw after LevelDB opened, like a failed reopen.
    int failures{0};
    const auto fail_opens = [&failures](const std::string* line) {
        if (line && failures > 0) {
            --failures;
            throw std::runtime_error("injected reopen failure");
        }
        return false;
    };

    // If the new size cannot be opened, the database is reopened with the previous size.
    failures = 1;
    {
        DebugLogHelper fail{"Opened LevelDB successfully", fail_opens};
        BOOST_CHECK_EXCEPTION(view.ResizeCache(1 << 21), std::runtime_error, HasReason("injected reopen failure"));
    }
    BOOST_CHECK_EQUAL(failures, 0);
    Coin coin;
    BOOST_CHECK(view.GetCoin(outpoint, coin));
    BOOST_CHECK_EQUAL(view.GetBestBlock(), best);

    // If that reopen fails as well, every access reports the closed database.
    failures = 2;
    {
        DebugLogHelper fail{"Opened LevelDB successfully", fail_opens};
        BOOST_CHECK_EXCEPTION(view.ResizeCache(1 << 21), std::runtime_error, HasReason("injected reopen failure"));
    }
    BOOST_CHECK_EQUAL(failures, 0);
    const HasReason closed{"Coin database is closed after a failed cache resize"};
    BOOST_CHECK_EXCEPTION(view.GetCoin(outpoint, coin), dbwrapper_error, closed);
    BOOST_CHECK_EXCEPTION(view.HaveCoin(outpoint), dbwrapper_error, closed);
    BOOST_CHECK_EXCEPTION(view.GetBestBlock(), dbwrapper_error, closed);
    BOOST_CHECK_EXCEPTION(view.GetHeadBlocks(), dbwrapper_error, closed);
    BOOST_CHECK_EXCEPTION(view.Cursor(), dbwrapper_error, closed);
    BOOST_CHECK_EXCEPTION(view.EstimateSize(), dbwrapper_error, closed);
    BOOST_CHECK_EXCEPTION(view.Upgrade(), dbwrapper_error, closed);
    CCoinsMapMemoryResource resource;
    CCoinsMap map{0, CCoinsMap::hasher{}, CCoinsMap::key_equal{}, &resource};
    BOOST_CHECK_EXCEPTION(view.BatchWrite(map, best), dbwrapper_error, closed);

    // A later resize reopens the database with its data intact.
    view.ResizeCache(1 << 21);
    BOOST_CHECK(view.GetCoin(outpoint, coin));
    BOOST_CHECK_EQUAL(view.GetBestBlock(), best);
}

//! A failed coins DB reopen must leave the recorded cache sizes unchanged, so the
//! same resize is attempted again instead of being reported as already applied.
BOOST_FIXTURE_TEST_CASE(chainstate_resize_reopen_failure, TestingSetup)
{
    ChainstateManager manager;
    WITH_LOCK(::cs_main, manager.m_blockman.m_block_tree_db = std::make_unique<CBlockTreeDB>(1 << 20, true));
    CTxMemPool mempool;

    CChainState& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool, *m_node.evodb, m_node.chain_helper));
    c1.InitCoinsDB(
        /* cache_size_bytes */ 1 << 23, /* in_memory */ false, /* should_wipe */ true, "chainstate_resize_failure");
    WITH_LOCK(::cs_main, c1.InitCoinsCache(1 << 23));
    BOOST_REQUIRE(c1.LoadGenesisBlock());

    LOCK(::cs_main);
    // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
    // flush during ResizeCoinsCaches() and will subsequently hit an assertion.
    c1.CoinsTip().SetBestBlock(InsecureRand256());
    const size_t coinstip_size{c1.m_coinstip_cache_size_bytes};
    const size_t coinsdb_size{c1.m_coinsdb_cache_size_bytes};

    // The first `failures` database opens throw after LevelDB opened, like a failed reopen.
    int failures{0};
    const auto fail_opens = [&failures](const std::string* line) {
        if (line && failures > 0) {
            --failures;
            throw std::runtime_error("injected reopen failure");
        }
        return false;
    };

    // Only the reopen with the new size fails; the previous size is restored.
    failures = 1;
    {
        DebugLogHelper fail_reopen{"Opened LevelDB successfully", fail_opens};
        BOOST_CHECK_EXCEPTION(c1.ResizeCoinsCaches(1 << 24, 1 << 22), std::runtime_error,
                              HasReason("injected reopen failure"));
    }
    BOOST_REQUIRE_EQUAL(failures, 0);
    BOOST_REQUIRE_EQUAL(c1.m_coinstip_cache_size_bytes, coinstip_size);
    BOOST_REQUIRE_EQUAL(c1.m_coinsdb_cache_size_bytes, coinsdb_size);
    BOOST_CHECK(c1.CoinsDB().GetHeadBlocks().empty());

    // Repeating the same request reopens the database and only then records the sizes.
    {
        ASSERT_DEBUG_LOG("Opened LevelDB successfully");
        BOOST_CHECK(c1.ResizeCoinsCaches(1 << 24, 1 << 22));
    }
    BOOST_CHECK_EQUAL(c1.m_coinstip_cache_size_bytes, size_t{1 << 24});
    BOOST_CHECK_EQUAL(c1.m_coinsdb_cache_size_bytes, size_t{1 << 22});

    // If the previous size cannot be restored either, the database stays closed and the
    // unchanged recorded sizes must not make a request for them look already applied.
    failures = 2;
    {
        DebugLogHelper fail_reopen{"Opened LevelDB successfully", fail_opens};
        BOOST_CHECK_EXCEPTION(c1.ResizeCoinsCaches(1 << 23, 1 << 23), std::runtime_error,
                              HasReason("injected reopen failure"));
    }
    BOOST_REQUIRE_EQUAL(failures, 0);
    BOOST_CHECK_EQUAL(c1.m_coinstip_cache_size_bytes, size_t{1 << 24});
    BOOST_CHECK_EQUAL(c1.m_coinsdb_cache_size_bytes, size_t{1 << 22});
    BOOST_CHECK_THROW(c1.CoinsDB().GetBestBlock(), dbwrapper_error);
    BOOST_CHECK(c1.ResizeCoinsCaches(1 << 24, 1 << 22));
    BOOST_CHECK_NO_THROW(c1.CoinsDB().GetBestBlock());
}

//! ReplayBlocks() treats only an absent head-blocks marker as a consistent coins
//! database; a present marker that does not decode stops it with an error.
BOOST_FIXTURE_TEST_CASE(chainstate_replay_head_blocks, TestingSetup)
{
    ChainstateManager manager;
    WITH_LOCK(::cs_main, manager.m_blockman.m_block_tree_db = std::make_unique<CBlockTreeDB>(1 << 20, true));
    CTxMemPool mempool;
    CChainState& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool, *m_node.evodb, m_node.chain_helper));

    // Open a fresh on-disk coins DB for the chainstate, with the marker written first.
    const auto open_with_marker = [&](const std::string& name, const auto& marker) {
        {
            CDBWrapper raw(gArgs.GetDataDirNet() / name, 1 << 20, /*fMemory=*/false, /*fWipe=*/true, /*obfuscate=*/true);
            BOOST_REQUIRE(raw.Write(uint8_t{'H'}, marker));
        }
        c1.InitCoinsDB(/* cache_size_bytes */ 1 << 20, /* in_memory */ false, /* should_wipe */ false, name);
    };

    c1.InitCoinsDB(/* cache_size_bytes */ 1 << 20, /* in_memory */ false, /* should_wipe */ true, "replay_absent");
    BOOST_CHECK(c1.ReplayBlocks());

    // Existing rejections of decodable markers are unchanged.
    open_with_marker("replay_one_head", std::vector<uint256>{InsecureRand256()});
    BOOST_CHECK(!c1.ReplayBlocks());
    open_with_marker("replay_unknown_heads", std::vector<uint256>{InsecureRand256(), InsecureRand256()});
    BOOST_CHECK(!c1.ReplayBlocks());

    // A two-entry marker with only one hash, and a bare two-entry count.
    open_with_marker("replay_truncated", std::make_pair(uint8_t{2}, InsecureRand256()));
    BOOST_CHECK_EXCEPTION(c1.ReplayBlocks(), dbwrapper_error, HasReason("Head blocks deserialization failure"));
    open_with_marker("replay_count_only", uint8_t{2});
    BOOST_CHECK_EXCEPTION(c1.ReplayBlocks(), dbwrapper_error, HasReason("Head blocks deserialization failure"));
    // A present marker that decodes to no block is not an absent marker either.
    open_with_marker("replay_empty", std::vector<uint256>{});
    BOOST_CHECK_EXCEPTION(c1.ReplayBlocks(), dbwrapper_error, HasReason("Head blocks marker is present but empty"));
}

BOOST_AUTO_TEST_CASE(dbwrapper)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_obfuscate_true" : "dbwrapper_obfuscate_false");
        CDBWrapper dbw(ph, (1 << 20), true, false, obfuscate);
        uint8_t key{'k'};
        uint256 in = InsecureRand256();
        uint256 res;

        // Ensure that we're doing real obfuscation when obfuscate=true
        BOOST_CHECK(obfuscate != is_null_key(dbwrapper_private::GetObfuscateKey(dbw)));

        BOOST_CHECK(dbw.Write(key, in));
        BOOST_CHECK(dbw.Read(key, res));
        BOOST_CHECK_EQUAL(res.ToString(), in.ToString());
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_basic_data)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_1_obfuscate_true" : "dbwrapper_1_obfuscate_false");
        CDBWrapper dbw(ph, (1 << 20), false, true, obfuscate);

        uint256 res;
        uint32_t res_uint_32;
        bool res_bool;

        // Ensure that we're doing real obfuscation when obfuscate=true
        BOOST_CHECK(obfuscate != is_null_key(dbwrapper_private::GetObfuscateKey(dbw)));

        //Simulate block raw data - "b + block hash"
        std::string key_block = "b" + InsecureRand256().ToString();

        uint256 in_block = InsecureRand256();
        BOOST_CHECK(dbw.Write(key_block, in_block));
        BOOST_CHECK(dbw.Read(key_block, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_block.ToString());

        //Simulate file raw data - "f + file_number"
        std::string key_file = strprintf("f%04x", InsecureRand32());

        uint256 in_file_info = InsecureRand256();
        BOOST_CHECK(dbw.Write(key_file, in_file_info));
        BOOST_CHECK(dbw.Read(key_file, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_file_info.ToString());

        //Simulate transaction raw data - "t + transaction hash"
        std::string key_transaction = "t" + InsecureRand256().ToString();

        uint256 in_transaction = InsecureRand256();
        BOOST_CHECK(dbw.Write(key_transaction, in_transaction));
        BOOST_CHECK(dbw.Read(key_transaction, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_transaction.ToString());

        //Simulate UTXO raw data - "c + transaction hash"
        std::string key_utxo = "c" + InsecureRand256().ToString();

        uint256 in_utxo = InsecureRand256();
        BOOST_CHECK(dbw.Write(key_utxo, in_utxo));
        BOOST_CHECK(dbw.Read(key_utxo, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_utxo.ToString());

        //Simulate last block file number - "l"
        uint8_t key_last_blockfile_number{'l'};
        uint32_t lastblockfilenumber = InsecureRand32();
        BOOST_CHECK(dbw.Write(key_last_blockfile_number, lastblockfilenumber));
        BOOST_CHECK(dbw.Read(key_last_blockfile_number, res_uint_32));
        BOOST_CHECK_EQUAL(lastblockfilenumber, res_uint_32);

        //Simulate Is Reindexing - "R"
        uint8_t key_IsReindexing{'R'};
        bool isInReindexing = InsecureRandBool();
        BOOST_CHECK(dbw.Write(key_IsReindexing, isInReindexing));
        BOOST_CHECK(dbw.Read(key_IsReindexing, res_bool));
        BOOST_CHECK_EQUAL(isInReindexing, res_bool);

        //Simulate last block hash up to which UXTO covers - 'B'
        uint8_t key_lastblockhash_uxto{'B'};
        uint256 lastblock_hash = InsecureRand256();
        BOOST_CHECK(dbw.Write(key_lastblockhash_uxto, lastblock_hash));
        BOOST_CHECK(dbw.Read(key_lastblockhash_uxto, res));
        BOOST_CHECK_EQUAL(lastblock_hash, res);

        //Simulate file raw data - "F + filename_number + filename"
        std::string file_option_tag = "F";
        uint8_t filename_length = InsecureRandBits(8);
        std::string filename = "randomfilename";
        std::string key_file_option = strprintf("%s%01x%s", file_option_tag,filename_length,filename);

        bool in_file_bool = InsecureRandBool();
        BOOST_CHECK(dbw.Write(key_file_option, in_file_bool));
        BOOST_CHECK(dbw.Read(key_file_option, res_bool));
        BOOST_CHECK_EQUAL(res_bool, in_file_bool);
   }
}

// Test batch operations
BOOST_AUTO_TEST_CASE(dbwrapper_batch)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_batch_obfuscate_true" : "dbwrapper_batch_obfuscate_false");
        CDBWrapper dbw(ph, (1 << 20), true, false, obfuscate);

        uint8_t key{'i'};
        uint256 in = InsecureRand256();
        uint8_t key2{'j'};
        uint256 in2 = InsecureRand256();
        uint8_t key3{'k'};
        uint256 in3 = InsecureRand256();

        uint256 res;
        CDBBatch batch(dbw);

        batch.Write(key, in);
        batch.Write(key2, in2);
        batch.Write(key3, in3);

        // Remove key3 before it's even been written
        batch.Erase(key3);

        BOOST_CHECK(dbw.WriteBatch(batch));

        BOOST_CHECK(dbw.Read(key, res));
        BOOST_CHECK_EQUAL(res.ToString(), in.ToString());
        BOOST_CHECK(dbw.Read(key2, res));
        BOOST_CHECK_EQUAL(res.ToString(), in2.ToString());

        // key3 should've never been written
        BOOST_CHECK(dbw.Read(key3, res) == false);
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_iterator)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_iterator_obfuscate_true" : "dbwrapper_iterator_obfuscate_false");
        CDBWrapper dbw(ph, (1 << 20), true, false, obfuscate);

        // The two keys are intentionally chosen for ordering
        uint8_t key{'j'};
        uint256 in = InsecureRand256();
        BOOST_CHECK(dbw.Write(key, in));
        uint8_t key2{'k'};
        uint256 in2 = InsecureRand256();
        BOOST_CHECK(dbw.Write(key2, in2));

        std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

        // Be sure to seek past the obfuscation key (if it exists)
        it->Seek(key);

        uint8_t key_res;
        uint256 val_res;

        BOOST_REQUIRE(it->GetKey(key_res));
        BOOST_REQUIRE(it->GetValue(val_res));
        BOOST_CHECK_EQUAL(key_res, key);
        BOOST_CHECK_EQUAL(val_res.ToString(), in.ToString());

        it->Next();

        BOOST_REQUIRE(it->GetKey(key_res));
        BOOST_REQUIRE(it->GetValue(val_res));
        BOOST_CHECK_EQUAL(key_res, key2);
        BOOST_CHECK_EQUAL(val_res.ToString(), in2.ToString());

        it->Next();
        BOOST_CHECK_EQUAL(it->Valid(), false);
    }
}

// Test that we do not obfuscation if there is existing data.
BOOST_AUTO_TEST_CASE(existing_data_no_obfuscate)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_no_obfuscate";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(ph, (1 << 10), false, false, false);
    uint8_t key{'k'};
    uint256 in = InsecureRand256();
    uint256 res;

    BOOST_CHECK(dbw->Write(key, in));
    BOOST_CHECK(dbw->Read(key, res));
    BOOST_CHECK_EQUAL(res.ToString(), in.ToString());

    // Call the destructor to free leveldb LOCK
    dbw.reset();

    // Now, set up another wrapper that wants to obfuscate the same directory
    CDBWrapper odbw(ph, (1 << 10), false, false, true);

    // Check that the key/val we wrote with unobfuscated wrapper exists and
    // is readable.
    uint256 res2;
    BOOST_CHECK(odbw.Read(key, res2));
    BOOST_CHECK_EQUAL(res2.ToString(), in.ToString());

    BOOST_CHECK(!odbw.IsEmpty()); // There should be existing data
    BOOST_CHECK(is_null_key(dbwrapper_private::GetObfuscateKey(odbw))); // The key should be an empty string

    uint256 in2 = InsecureRand256();
    uint256 res3;

    // Check that we can write successfully
    BOOST_CHECK(odbw.Write(key, in2));
    BOOST_CHECK(odbw.Read(key, res3));
    BOOST_CHECK_EQUAL(res3.ToString(), in2.ToString());
}

// Ensure that we start obfuscating during a reindex.
BOOST_AUTO_TEST_CASE(existing_data_reindex)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_reindex";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(ph, (1 << 10), false, false, false);
    uint8_t key{'k'};
    uint256 in = InsecureRand256();
    uint256 res;

    BOOST_CHECK(dbw->Write(key, in));
    BOOST_CHECK(dbw->Read(key, res));
    BOOST_CHECK_EQUAL(res.ToString(), in.ToString());

    // Call the destructor to free leveldb LOCK
    dbw.reset();

    // Simulate a -reindex by wiping the existing data store
    CDBWrapper odbw(ph, (1 << 10), false, true, true);

    // Check that the key/val we wrote with unobfuscated wrapper doesn't exist
    uint256 res2;
    BOOST_CHECK(!odbw.Read(key, res2));
    BOOST_CHECK(!is_null_key(dbwrapper_private::GetObfuscateKey(odbw)));

    uint256 in2 = InsecureRand256();
    uint256 res3;

    // Check that we can write successfully
    BOOST_CHECK(odbw.Write(key, in2));
    BOOST_CHECK(odbw.Read(key, res3));
    BOOST_CHECK_EQUAL(res3.ToString(), in2.ToString());
}

BOOST_AUTO_TEST_CASE(iterator_ordering)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_ordering";
    CDBWrapper dbw(ph, (1 << 20), true, false, false);
    for (int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (!(x & 1)) BOOST_CHECK(dbw.Write(key, value));
    }

    // Check that creating an iterator creates a snapshot
    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

    for (unsigned int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (x & 1) BOOST_CHECK(dbw.Write(key, value));
    }

    for (const int seek_start : {0x00, 0x80}) {
        it->Seek((uint8_t)seek_start);
        for (unsigned int x=seek_start; x<255; ++x) {
            uint8_t key;
            uint32_t value;
            BOOST_CHECK(it->Valid());
            if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                break;
            BOOST_CHECK(it->GetKey(key));
            if (x & 1) {
                BOOST_CHECK_EQUAL(key, x + 1);
                continue;
            }
            BOOST_CHECK(it->GetValue(value));
            BOOST_CHECK_EQUAL(key, x);
            BOOST_CHECK_EQUAL(value, x*x);
            it->Next();
        }
        BOOST_CHECK(!it->Valid());
    }
}

struct StringContentsSerializer {
    // Used to make two serialized objects the same while letting them have different lengths
    // This is a terrible idea
    std::string str;
    StringContentsSerializer() {}
    explicit StringContentsSerializer(const std::string& inp) : str(inp) {}

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        for (size_t i = 0; i < str.size(); i++) {
            s << uint8_t(str[i]);
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        str.clear();
        uint8_t c{0};
        while (!s.eof()) {
            s >> c;
            str.push_back(c);
        }
    }
};

BOOST_AUTO_TEST_CASE(iterator_string_ordering)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_string_ordering";
    CDBWrapper dbw(ph, (1 << 20), true, false, false);
    for (int x = 0; x < 10; ++x) {
        for (int y = 0; y < 10; ++y) {
            std::string key{ToString(x)};
            for (int z = 0; z < y; ++z)
                key += key;
            uint32_t value = x*x;
            BOOST_CHECK(dbw.Write(StringContentsSerializer{key}, value));
        }
    }

    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());
    for (const int seek_start : {0, 5}) {
        it->Seek(StringContentsSerializer{ToString(seek_start)});
        for (unsigned int x = seek_start; x < 10; ++x) {
            for (int y = 0; y < 10; ++y) {
                std::string exp_key{ToString(x)};
                for (int z = 0; z < y; ++z)
                    exp_key += exp_key;
                StringContentsSerializer key;
                uint32_t value;
                BOOST_CHECK(it->Valid());
                if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                    break;
                BOOST_CHECK(it->GetKey(key));
                BOOST_CHECK(it->GetValue(value));
                BOOST_CHECK_EQUAL(key.str, exp_key);
                BOOST_CHECK_EQUAL(value, x*x);
                it->Next();
            }
        }
        BOOST_CHECK(!it->Valid());
    }
}

BOOST_AUTO_TEST_CASE(unicodepath)
{
    // Attempt to create a database with a UTF8 character in the path.
    // On Windows this test will fail if the directory is created using
    // the ANSI CreateDirectoryA call and the code page isn't UTF8.
    // It will succeed if created with CreateDirectoryW.
    fs::path ph = m_args.GetDataDirBase() / "test_runner_∋_🏃_20191128_104644";
    CDBWrapper dbw(ph, (1 << 20));

    fs::path lockPath = ph / "LOCK";
    BOOST_CHECK(fs::exists(lockPath));
}


BOOST_AUTO_TEST_CASE(dbwrapper_transaction_exception_safety)
{
    const fs::path path = m_args.GetDataDirBase() / "dbwrapper_transaction_exception_safety";
    CDBWrapper dbw(path, (1 << 20), true, true, false);
    CDBBatch batch(dbw);
    using RootTransaction = CDBTransaction<CDBWrapper, CDBBatch>;
    using CurTransaction = CDBTransaction<RootTransaction, RootTransaction>;

    // A failed holder copy must not publish an empty write slot.
    {
        RootTransaction transaction(dbw, batch);
        const uint8_t key{1};
        const CopyFaultValue value{10};
        CopyFaultValue result;
        bool read{true};
        const size_t usage_before = transaction.GetMemoryUsage();

        CopyFaultValue::s_copies_before_throw = 0;
        BOOST_CHECK_THROW(transaction.Write(key, value), std::runtime_error);
        CopyFaultValue::s_copies_before_throw = -1;

        BOOST_CHECK_EQUAL(transaction.GetMemoryUsage(), usage_before);
        BOOST_CHECK(!transaction.Exists(key));
        BOOST_CHECK_NO_THROW(read = transaction.Read(key, result));
        BOOST_CHECK(!read);
    }

    // A failed replacement must preserve both the old value and its accounting.
    {
        RootTransaction transaction(dbw, batch);
        const uint8_t key{2};
        const CopyFaultValue original{20};
        const CopyFaultValue replacement{21};
        CopyFaultValue result;
        transaction.Write(key, original);
        const size_t usage_before = transaction.GetMemoryUsage();

        CopyFaultValue::s_copies_before_throw = 0;
        BOOST_CHECK_THROW(transaction.Write(key, replacement), std::runtime_error);
        CopyFaultValue::s_copies_before_throw = -1;

        bool read{false};
        BOOST_CHECK_EQUAL(transaction.GetMemoryUsage(), usage_before);
        BOOST_CHECK_NO_THROW(read = transaction.Read(key, result));
        BOOST_CHECK(read);
        BOOST_CHECK_EQUAL(result.m_value, original.m_value);
    }

    // A failed write after Erase must retain the tombstone, not expose the parent value.
    {
        const uint8_t key{3};
        const CopyFaultValue original{30};
        const CopyFaultValue replacement{31};
        BOOST_REQUIRE(dbw.Write(key, original));
        RootTransaction transaction(dbw, batch);
        transaction.Erase(key);
        const size_t usage_before = transaction.GetMemoryUsage();
        BOOST_CHECK(!transaction.Exists(key));

        CopyFaultValue::s_copies_before_throw = 0;
        BOOST_CHECK_THROW(transaction.Write(key, replacement), std::runtime_error);
        CopyFaultValue::s_copies_before_throw = -1;

        CopyFaultValue result;
        bool read{true};
        BOOST_CHECK_EQUAL(transaction.GetMemoryUsage(), usage_before);
        BOOST_CHECK(!transaction.Exists(key));
        BOOST_CHECK_NO_THROW(read = transaction.Read(key, result));
        BOOST_CHECK(!read);
    }

    // A failure at the first or a later staged write must leave both overlays
    // unchanged; the child must remain reusable for a successful retry.
    {
        RootTransaction root(dbw, batch);
        CurTransaction current(root, root);
        const uint8_t root_tombstone_key{4};
        const uint8_t replaced_key{5};
        const uint8_t inserted_key_1{6};
        const uint8_t inserted_key_2{7};
        const uint8_t erased_key{8};
        const uint8_t preserved_key{9};
        const uint8_t root_tombstone_key_2{10};
        const CopyFaultValue original{40};
        const CopyFaultValue tombstone_replacement{42};
        const CopyFaultValue replacement{41};
        const CopyFaultValue inserted_1{61};
        const CopyFaultValue inserted_2{71};
        const CopyFaultValue erased_value{80};
        const CopyFaultValue preserved{90};

        root.Erase(root_tombstone_key);
        root.Erase(root_tombstone_key_2);
        root.Write(replaced_key, original);
        root.Write(erased_key, erased_value);
        root.Write(preserved_key, preserved);
        current.Write(root_tombstone_key, tombstone_replacement);
        current.Write(replaced_key, replacement);
        current.Write(inserted_key_1, inserted_1);
        current.Write(inserted_key_2, inserted_2);
        current.Erase(erased_key);
        current.Erase(root_tombstone_key_2);
        auto replaced_iterator = root.NewIteratorUniquePtr();
        replaced_iterator->Seek(replaced_key);
        auto preserved_iterator = root.NewIteratorUniquePtr();
        preserved_iterator->Seek(preserved_key);
        const size_t root_usage_before = root.GetMemoryUsage();
        const size_t current_usage_before = current.GetMemoryUsage();

        const auto check_unmodified = [&]() {
            CopyFaultValue result;
            BOOST_CHECK_EQUAL(root.GetMemoryUsage(), root_usage_before);
            BOOST_CHECK_EQUAL(current.GetMemoryUsage(), current_usage_before);
            BOOST_CHECK(!root.Exists(root_tombstone_key));
            BOOST_CHECK(!root.Exists(root_tombstone_key_2));
            BOOST_CHECK(!root.Exists(inserted_key_1));
            BOOST_CHECK(!root.Exists(inserted_key_2));
            BOOST_CHECK(root.Read(replaced_key, result));
            BOOST_CHECK_EQUAL(result.m_value, original.m_value);
            BOOST_CHECK(root.Read(erased_key, result));
            BOOST_CHECK_EQUAL(result.m_value, erased_value.m_value);
            BOOST_CHECK(root.Read(preserved_key, result));
            BOOST_CHECK_EQUAL(result.m_value, preserved.m_value);
            BOOST_CHECK(current.Read(root_tombstone_key, result));
            BOOST_CHECK_EQUAL(result.m_value, tombstone_replacement.m_value);
            BOOST_CHECK(current.Read(replaced_key, result));
            BOOST_CHECK_EQUAL(result.m_value, replacement.m_value);
            BOOST_CHECK(current.Read(inserted_key_1, result));
            BOOST_CHECK_EQUAL(result.m_value, inserted_1.m_value);
            BOOST_CHECK(current.Read(inserted_key_2, result));
            BOOST_CHECK_EQUAL(result.m_value, inserted_2.m_value);
            BOOST_CHECK(!current.Exists(erased_key));
            BOOST_CHECK(!current.Read(erased_key, result));
            BOOST_CHECK(!current.Exists(root_tombstone_key_2));
        };

        CopyFaultValue::s_copies_before_throw = 0;
        BOOST_CHECK_THROW(current.Commit(), std::runtime_error);
        CopyFaultValue::s_copies_before_throw = -1;
        check_unmodified();

        // The first staged write succeeds and the second throws.
        CopyFaultValue::s_copies_before_throw = 1;
        BOOST_CHECK_THROW(current.Commit(), std::runtime_error);
        CopyFaultValue::s_copies_before_throw = -1;
        check_unmodified();

        BOOST_CHECK_NO_THROW(current.Commit());
        CopyFaultValue result;
        BOOST_CHECK(root.Read(root_tombstone_key, result));
        BOOST_CHECK_EQUAL(result.m_value, tombstone_replacement.m_value);
        BOOST_CHECK(!root.Exists(root_tombstone_key_2));
        BOOST_CHECK(root.Read(replaced_key, result));
        BOOST_CHECK_EQUAL(result.m_value, replacement.m_value);
        BOOST_CHECK(root.Read(inserted_key_1, result));
        BOOST_CHECK_EQUAL(result.m_value, inserted_1.m_value);
        BOOST_CHECK(root.Read(inserted_key_2, result));
        BOOST_CHECK_EQUAL(result.m_value, inserted_2.m_value);
        BOOST_CHECK(!root.Exists(erased_key));
        BOOST_CHECK(root.Read(preserved_key, result));
        BOOST_CHECK_EQUAL(result.m_value, preserved.m_value);
        BOOST_CHECK_EQUAL(root.GetMemoryUsage(), sizeof(uint8_t) * 7 +
                          ::GetSerializeSize(tombstone_replacement, CLIENT_VERSION) +
                          ::GetSerializeSize(replacement, CLIENT_VERSION) +
                          ::GetSerializeSize(inserted_1, CLIENT_VERSION) +
                          ::GetSerializeSize(inserted_2, CLIENT_VERSION) +
                          ::GetSerializeSize(preserved, CLIENT_VERSION));
        BOOST_CHECK_EQUAL(current.GetMemoryUsage(), 0);

        uint8_t iterated_key{0};
        CopyFaultValue iterated_value;
        BOOST_CHECK(replaced_iterator->Valid());
        BOOST_CHECK(replaced_iterator->GetKey(iterated_key));
        BOOST_CHECK_EQUAL(iterated_key, replaced_key);
        BOOST_CHECK(replaced_iterator->GetValue(iterated_value));
        BOOST_CHECK_EQUAL(iterated_value.m_value, replacement.m_value);
        BOOST_CHECK(preserved_iterator->Valid());
        BOOST_CHECK(preserved_iterator->GetKey(iterated_key));
        BOOST_CHECK_EQUAL(iterated_key, preserved_key);
        BOOST_CHECK(preserved_iterator->GetValue(iterated_value));
        BOOST_CHECK_EQUAL(iterated_value.m_value, preserved.m_value);
    }

    // A successful child commit must preserve other parent entries and apply both writes and erases.
    {
        RootTransaction root(dbw, batch);
        CurTransaction current(root, root);
        const uint8_t overwritten_key{5};
        const uint8_t erased_key{6};
        const uint8_t inserted_key{7};
        const CopyFaultValue original{50};
        const CopyFaultValue replacement{51};
        const CopyFaultValue preserved{60};
        const CopyFaultValue inserted{70};
        CopyFaultValue result;

        root.Write(overwritten_key, original);
        root.Write(erased_key, preserved);
        current.Write(overwritten_key, replacement);
        current.Erase(erased_key);
        current.Write(inserted_key, inserted);
        current.Commit();

        bool read{false};
        BOOST_CHECK_NO_THROW(read = root.Read(overwritten_key, result));
        BOOST_CHECK(read);
        BOOST_CHECK_EQUAL(result.m_value, replacement.m_value);
        BOOST_CHECK(!root.Exists(erased_key));
        BOOST_CHECK_NO_THROW(read = root.Read(inserted_key, result));
        BOOST_CHECK(read);
        BOOST_CHECK_EQUAL(result.m_value, inserted.m_value);
        BOOST_CHECK_EQUAL(root.GetMemoryUsage(), sizeof(uint8_t) * 3 +
                          ::GetSerializeSize(replacement, CLIENT_VERSION) +
                          ::GetSerializeSize(inserted, CLIENT_VERSION));
        BOOST_CHECK_EQUAL(current.GetMemoryUsage(), 0);
    }
}


BOOST_AUTO_TEST_CASE(dbwrapper_address_index_type_boundary)
{
    uint160 last_hash;
    last_hash.SetHex(std::string(40, 'f'));
    uint256 type1_tx;
    type1_tx.SetHex(std::string(63, '0') + '1');
    uint256 type2_tx;
    type2_tx.SetHex(std::string(63, '0') + '2');

    // With no type-1 row for the maximum hash, the first matching hash row is type 2.
    {
        CBlockTreeDB db(1 << 20, true, true);
        std::vector<CAddressIndexEntry> history{
            {CAddressIndexKey(AddressType::P2SH, last_hash, 10, 0, type2_tx, 0, false), 200},
        };
        std::vector<CAddressUnspentIndexEntry> unspent{
            {CAddressUnspentKey(AddressType::P2SH, last_hash, type2_tx, 0),
             CAddressUnspentValue(200, CScript{}, 10)},
        };
        BOOST_REQUIRE(db.WriteAddressIndex(history));
        BOOST_REQUIRE(db.UpdateAddressUnspentIndex(unspent));

        std::vector<CAddressIndexEntry> type1_history;
        BOOST_REQUIRE(db.ReadAddressIndex(last_hash, AddressType::P2PK_OR_P2PKH, type1_history));
        BOOST_CHECK(type1_history.empty());
        BOOST_REQUIRE(db.ReadAddressIndex(last_hash, AddressType::P2PK_OR_P2PKH, type1_history, 1, 20));
        BOOST_CHECK(type1_history.empty());
        std::vector<CAddressUnspentIndexEntry> type1_unspent;
        BOOST_REQUIRE(db.ReadAddressUnspentIndex(last_hash, AddressType::P2PK_OR_P2PKH, type1_unspent));
        BOOST_CHECK(type1_unspent.empty());
    }

    // A type-1 range at the end of its hash space must stop before type 2 of the same hash.
    {
        CBlockTreeDB db(1 << 20, true, true);
        std::vector<CAddressIndexEntry> history{
            {CAddressIndexKey(AddressType::P2PK_OR_P2PKH, last_hash, 10, 0, type1_tx, 0, false), 100},
            {CAddressIndexKey(AddressType::P2SH, last_hash, 11, 0, type2_tx, 0, false), 200},
        };
        std::vector<CAddressUnspentIndexEntry> unspent{
            {CAddressUnspentKey(AddressType::P2PK_OR_P2PKH, last_hash, type1_tx, 0),
             CAddressUnspentValue(100, CScript{}, 10)},
            {CAddressUnspentKey(AddressType::P2SH, last_hash, type2_tx, 0),
             CAddressUnspentValue(200, CScript{}, 11)},
        };
        BOOST_REQUIRE(db.WriteAddressIndex(history));
        BOOST_REQUIRE(db.UpdateAddressUnspentIndex(unspent));

        std::vector<CAddressIndexEntry> type1_history;
        BOOST_REQUIRE(db.ReadAddressIndex(last_hash, AddressType::P2PK_OR_P2PKH, type1_history));
        BOOST_REQUIRE_EQUAL(type1_history.size(), 1);
        BOOST_CHECK(type1_history[0].first.m_address_type == AddressType::P2PK_OR_P2PKH);
        BOOST_CHECK_EQUAL(type1_history[0].second, 100);
        type1_history.clear();
        BOOST_REQUIRE(db.ReadAddressIndex(last_hash, AddressType::P2PK_OR_P2PKH, type1_history, 10, 11));
        BOOST_REQUIRE_EQUAL(type1_history.size(), 1);
        BOOST_CHECK_EQUAL(type1_history[0].second, 100);

        std::vector<CAddressUnspentIndexEntry> type1_unspent;
        BOOST_REQUIRE(db.ReadAddressUnspentIndex(last_hash, AddressType::P2PK_OR_P2PKH, type1_unspent));
        BOOST_REQUIRE_EQUAL(type1_unspent.size(), 1);
        BOOST_CHECK(type1_unspent[0].first.m_address_type == AddressType::P2PK_OR_P2PKH);
        BOOST_CHECK_EQUAL(type1_unspent[0].second.m_amount, 100);
    }

    // A different-hash type-1 record still terminates a sparse scan before type 2.
    {
        uint160 target_hash;
        target_hash.SetHex(std::string(39, '0') + '1');
        uint160 next_hash;
        next_hash.SetHex(std::string(39, '0') + '2');
        CBlockTreeDB db(1 << 20, true, true);
        std::vector<CAddressIndexEntry> history{
            {CAddressIndexKey(AddressType::P2SH, target_hash, 10, 0, type2_tx, 0, false), 200},
            {CAddressIndexKey(AddressType::P2PK_OR_P2PKH, next_hash, 11, 0, type1_tx, 0, false), 100},
        };
        BOOST_REQUIRE(db.WriteAddressIndex(history));

        std::vector<CAddressIndexEntry> type1_history;
        BOOST_REQUIRE(db.ReadAddressIndex(target_hash, AddressType::P2PK_OR_P2PKH, type1_history));
        BOOST_CHECK(type1_history.empty());
    }
}


BOOST_AUTO_TEST_SUITE_END()
