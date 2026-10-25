// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <txdb.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/string.h>

#include <memory>
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
} // namespace

BOOST_FIXTURE_TEST_SUITE(dbwrapper_tests, BasicTestingSetup)

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
