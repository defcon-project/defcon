// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <clientversion.h>
#include <fs.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <wallet/bdb.h>

#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string Bytes(const CDataStream& stream) { return std::string{reinterpret_cast<const char*>(stream.data()), stream.size()}; }

template <typename V>
std::string Stored(const V& value)
{
    CDataStream stream(SER_DISK, CLIENT_VERSION);
    stream << value;
    return Bytes(stream);
}

//! A batch over a database file whose cursor does not open.
class NoCursorBatch : public BerkeleyBatch
{
public:
    using BerkeleyBatch::BerkeleyBatch;
    bool StartCursor() override { return false; }
};

//! A database file whose Rewrite reads the original through a batch whose cursor does not open, when told to.
class FileDatabase : public BerkeleyDatabase
{
public:
    using BerkeleyDatabase::BerkeleyDatabase;
    bool cursor_opens{true};

protected:
    std::unique_ptr<BerkeleyBatch> MakeRewriteSource() override
    {
        if (!cursor_opens) return std::make_unique<NoCursorBatch>(*this, true);
        return BerkeleyDatabase::MakeRewriteSource();
    }
};

const std::string FILE_NAME{"wallet.dat"};
const auto POOL_KEY{std::make_pair(std::string{"pool"}, int64_t{7})};

//! Four records written to a database file: the version (not this client's, so that a file rewritten after all
//! shows it), one of the type a rewrite can be told to skip, and two others.
void WriteFourRecords(WalletDatabase& database)
{
    std::unique_ptr<DatabaseBatch> batch = database.MakeBatch();
    BOOST_REQUIRE(batch->Write(std::string{"version"}, 1));
    BOOST_REQUIRE(batch->Write(POOL_KEY, std::string{"left out"}));
    BOOST_REQUIRE(batch->Write(std::string{"kept"}, std::string{"one"}));
    BOOST_REQUIRE(batch->Write(std::string{"poolish"}, std::string{"another type, kept"}));
}

//! Every record of a database, key and value, as its cursor gives them.
std::vector<std::pair<std::string, std::string>> RecordsOf(WalletDatabase& database)
{
    std::vector<std::pair<std::string, std::string>> records;
    std::unique_ptr<DatabaseBatch> batch = database.MakeBatch();
    BOOST_REQUIRE(batch->StartCursor());
    while (true) {
        CDataStream key(SER_DISK, CLIENT_VERSION);
        CDataStream value(SER_DISK, CLIENT_VERSION);
        bool complete;
        if (!batch->ReadAtCursor(key, value, complete)) {
            BOOST_REQUIRE(complete);
            break;
        }
        records.emplace_back(Bytes(key), Bytes(value));
    }
    batch->CloseCursor();
    return records;
}

//! The same records as a rewrite leaves them: the version record carries this client's version, and the records
//! whose key starts with `skip` are left out.
std::vector<std::pair<std::string, std::string>> Rewritten(std::vector<std::pair<std::string, std::string>> records, const std::string& skip = "")
{
    std::vector<std::pair<std::string, std::string>> rewritten;
    for (auto& [key, value] : records) {
        if (!skip.empty() && key.compare(0, skip.size(), skip) == 0) continue;
        if (key == Stored(std::string{"version"})) value = Stored(CLIENT_VERSION);
        rewritten.emplace_back(key, value);
    }
    return rewritten;
}

//! The records of the database file in a folder, opened anew under its name, in an environment of its own.
std::vector<std::pair<std::string, std::string>> RecordsOfFileIn(const fs::path& dir)
{
    BerkeleyDatabase database(GetBerkeleyEnv(dir), FILE_NAME);
    return RecordsOf(database);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(db_tests, BasicTestingSetup)

static std::shared_ptr<BerkeleyEnvironment> GetWalletEnv(const fs::path& path, std::string& database_filename)
{
    fs::path data_file = BDBDataFile(path);
    database_filename = fs::PathToString(data_file.filename());
    return GetBerkeleyEnv(data_file.parent_path());
}

BOOST_AUTO_TEST_CASE(getwalletenv_file)
{
    std::string test_name = "test_name.dat";
    const fs::path datadir = gArgs.GetDataDirNet();
    fs::path file_path = datadir / test_name;
    std::ofstream f{file_path};
    f.close();

    std::string filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(file_path, filename);
    BOOST_CHECK_EQUAL(filename, test_name);
    BOOST_CHECK_EQUAL(env->Directory(), datadir);
}

BOOST_AUTO_TEST_CASE(getwalletenv_directory)
{
    std::string expected_name = "wallet.dat";
    const fs::path datadir = gArgs.GetDataDirNet();

    std::string filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(datadir, filename);
    BOOST_CHECK_EQUAL(filename, expected_name);
    BOOST_CHECK_EQUAL(env->Directory(), datadir);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_multiple)
{
    fs::path datadir = gArgs.GetDataDirNet() / "1";
    fs::path datadir_2 = gArgs.GetDataDirNet() / "2";
    std::string filename;

    std::shared_ptr<BerkeleyEnvironment> env_1 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_3 = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1 == env_2);
    BOOST_CHECK(env_2 != env_3);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_free_instance)
{
    fs::path datadir = gArgs.GetDataDirNet() / "1";
    fs::path datadir_2 = gArgs.GetDataDirNet() / "2";
    std::string filename;

    std::shared_ptr <BerkeleyEnvironment> env_1_a = GetWalletEnv(datadir, filename);
    std::shared_ptr <BerkeleyEnvironment> env_2_a = GetWalletEnv(datadir_2, filename);
    env_1_a.reset();

    std::shared_ptr<BerkeleyEnvironment> env_1_b = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2_b = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1_a != env_1_b);
    BOOST_CHECK(env_2_a == env_2_b);
}

// Rewrite on a database file: every record, key and value, the version this client's; told to skip a type, without
// its records. Opened anew, the file under its name holds the same.
BOOST_AUTO_TEST_CASE(a_rewritten_database_file_holds_its_records)
{
    const fs::path dir = gArgs.GetDataDirNet() / "rewritten";
    std::vector<std::pair<std::string, std::string>> written;
    {
        FileDatabase database(GetBerkeleyEnv(dir), FILE_NAME);
        WriteFourRecords(database);
        written = RecordsOf(database);
        BOOST_REQUIRE_EQUAL(written.size(), 4U);
        BOOST_REQUIRE(database.Rewrite());
        BOOST_CHECK(RecordsOf(database) == Rewritten(written));
    }
    BOOST_CHECK(RecordsOfFileIn(dir) == Rewritten(written));
    {
        FileDatabase database(GetBerkeleyEnv(dir), FILE_NAME);
        BOOST_REQUIRE(database.Rewrite("\x04pool"));
        BOOST_CHECK(RecordsOf(database) == Rewritten(written, Stored(std::string{"pool"})));
    }
    BOOST_CHECK(RecordsOfFileIn(dir) == Rewritten(written, Stored(std::string{"pool"})));
}

// Rewrite on a database file when the cursor over the original does not open: it fails, and the file under the
// original's name still holds every record, key and value, also when it is opened anew. Before the change the copy
// loop was left out, the rewrite counted as a success, and the empty copy took the original's place.
BOOST_AUTO_TEST_CASE(a_rewrite_whose_cursor_does_not_open_leaves_the_file)
{
    const fs::path dir = gArgs.GetDataDirNet() / "nocursor";
    std::vector<std::pair<std::string, std::string>> written;
    {
        FileDatabase database(GetBerkeleyEnv(dir), FILE_NAME);
        WriteFourRecords(database);
        written = RecordsOf(database);
        BOOST_REQUIRE_EQUAL(written.size(), 4U);

        database.cursor_opens = false;
        BOOST_CHECK(!database.Rewrite());
        BOOST_CHECK(RecordsOf(database) == written);
        BOOST_CHECK(!database.Rewrite("\x04pool"));
        BOOST_CHECK(RecordsOf(database) == written);
    }
    BOOST_CHECK(RecordsOfFileIn(dir) == written);

    // With a cursor that opens the same file is rewritten.
    {
        FileDatabase database(GetBerkeleyEnv(dir), FILE_NAME);
        BOOST_CHECK(database.Rewrite());
        BOOST_CHECK(RecordsOf(database) == Rewritten(written));
    }
    BOOST_CHECK(RecordsOfFileIn(dir) == Rewritten(written));
}

BOOST_AUTO_TEST_SUITE_END()
