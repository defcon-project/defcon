// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <coins.h>
#include <compressor.h>
#include <dbwrapper.h>
#include <fs.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

/* F-2026-113. The pre-0.15 per-transaction utxo record -- CCoins in txdb.cpp --
 * has no fCoinStake field on disk: the format predates the fork, and its reader
 * correctly leaves the member alone. CCoinsViewDB::Upgrade() nevertheless copies
 * that member into every Coin it migrates, so until the constructor initialised
 * it the migrated coin carried whatever the stack held, and a coin marked as a
 * coinstake by accident is a coin the maturity rule holds back for 25 blocks.
 *
 * The fixture writes legacy records the way 0.8-0.14 wrote them, runs the
 * upgrade, and reads the coins back: fCoinStake must be false on every one of
 * them, and everything the record does carry -- value, script, height, the
 * coinbase flag -- must arrive intact. The reader lives in an anonymous
 * namespace, so the writer here is its mirror image, kept as small as these
 * records need: two outputs, both unspent, which sets bits 1 and 2 of the
 * header code and so leaves no spentness-mask bytes to write.
 *
 * An uninitialised stack slot in a fresh process may well hold a zero, which
 * would let the unfixed reader pass this test by luck, so the fixture dirties
 * the stack region Upgrade() is about to use with 0xFF bytes first. 0xFF is
 * deliberate: the compiler reads a bool through its low bit, so a pattern such
 * as 0xFE (what -ftrivial-auto-var-init=pattern plants) still reads as false and
 * shows nothing -- measured before this helper was written. A reader that
 * initialises the member is indifferent to what the stack held. */
namespace {
struct LegacyCoinsRecord {
    bool fCoinBase{false};
    CTxOut out0;
    CTxOut out1;
    int nHeight{0};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        unsigned int nVersion{1};
        ::Serialize(s, VARINT(nVersion));
        // bit 0: coinbase; bits 1 and 2: vout[0] and vout[1] unspent. With either of
        // the two set, the reader computes nMaskCode = 0 and reads no mask bytes.
        unsigned int nCode{(fCoinBase ? 1u : 0u) | 2u | 4u};
        ::Serialize(s, VARINT(nCode));
        ::Serialize(s, Using<TxOutCompression>(out0));
        ::Serialize(s, Using<TxOutCompression>(out1));
        ::Serialize(s, VARINT_MODE(nHeight, VarIntMode::NONNEGATIVE_SIGNED));
    }
};

//! txdb.cpp's DB_COINS, the key prefix of the pre-0.15 per-transaction records.
constexpr uint8_t LEGACY_DB_COINS{'c'};

//! Fill the stack below the caller's frame with 0xFF, so that an automatic the
//! callee never initialises is caught holding a non-zero byte.
__attribute__((noinline)) void DirtyStack()
{
    volatile unsigned char buf[65536];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = 0xFF;
    }
}

CTxOut MakeOut(CAmount value, unsigned char tag)
{
    return CTxOut(value, CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, tag) << OP_EQUALVERIFY << OP_CHECKSIG);
}

void CheckMigrated(const CCoinsViewDB& view, const uint256& txid, uint32_t n, const CTxOut& expected_out, int expected_height, bool expected_coinbase)
{
    Coin coin;
    BOOST_REQUIRE_MESSAGE(view.GetCoin(COutPoint(txid, n), coin), "migrated coin missing: " << txid.ToString() << ":" << n);
    BOOST_CHECK_EQUAL(coin.IsCoinStake(), false);
    BOOST_CHECK_EQUAL(coin.IsCoinBase(), expected_coinbase);
    BOOST_CHECK_EQUAL(coin.nHeight, expected_height);
    BOOST_CHECK(coin.out == expected_out);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(txdb_upgrade_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(legacy_coins_migrate_with_fcoinstake_false)
{
    const fs::path path{m_args.GetDataDirBase() / "txdb_upgrade_legacy_coins"};
    const uint256 txid_plain{uint256S("0x1111111111111111111111111111111111111111111111111111111111111111")};
    const uint256 txid_coinbase{uint256S("0x2222222222222222222222222222222222222222222222222222222222222222")};
    const CTxOut plain0{MakeOut(1 * COIN, 0x11)};
    const CTxOut plain1{MakeOut(2 * COIN, 0x22)};
    const CTxOut coinbase0{MakeOut(3 * COIN, 0x33)};
    const CTxOut coinbase1{MakeOut(4 * COIN, 0x44)};

    // 1. A database holding nothing but two legacy records, written with the
    //    same obfuscation CCoinsViewDB will open it with.
    {
        CDBWrapper legacy{path, 1 << 20, /*fMemory=*/false, /*fWipe=*/true, /*obfuscate=*/true};
        LegacyCoinsRecord plain;
        plain.fCoinBase = false;
        plain.out0 = plain0;
        plain.out1 = plain1;
        plain.nHeight = 123;
        LegacyCoinsRecord coinbase;
        coinbase.fCoinBase = true;
        coinbase.out0 = coinbase0;
        coinbase.out1 = coinbase1;
        coinbase.nHeight = 456;
        BOOST_REQUIRE(legacy.Write(std::make_pair(LEGACY_DB_COINS, txid_plain), plain));
        BOOST_REQUIRE(legacy.Write(std::make_pair(LEGACY_DB_COINS, txid_coinbase), coinbase));
        BOOST_CHECK(legacy.Exists(std::make_pair(LEGACY_DB_COINS, txid_plain)));
        BOOST_CHECK(legacy.Exists(std::make_pair(LEGACY_DB_COINS, txid_coinbase)));
    }

    // 2. The upgrade every node runs at startup (node/chainstate.cpp), on a
    //    dirty stack, and what it produced: four per-output coins, none of
    //    them a coinstake.
    {
        CCoinsViewDB view{path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false};
        DirtyStack();
        const bool upgraded{view.Upgrade()};
        BOOST_REQUIRE(upgraded);
        CheckMigrated(view, txid_plain, 0, plain0, 123, /*expected_coinbase=*/false);
        CheckMigrated(view, txid_plain, 1, plain1, 123, /*expected_coinbase=*/false);
        CheckMigrated(view, txid_coinbase, 0, coinbase0, 456, /*expected_coinbase=*/true);
        CheckMigrated(view, txid_coinbase, 1, coinbase1, 456, /*expected_coinbase=*/true);
        BOOST_CHECK(!view.HaveCoin(COutPoint(txid_plain, 2)));
        BOOST_CHECK(!view.HaveCoin(COutPoint(txid_coinbase, 2)));
    }

    // 3. The legacy records are gone, so a second start has nothing to migrate.
    {
        CDBWrapper after{path, 1 << 20, /*fMemory=*/false, /*fWipe=*/false, /*obfuscate=*/true};
        BOOST_CHECK(!after.Exists(std::make_pair(LEGACY_DB_COINS, txid_plain)));
        BOOST_CHECK(!after.Exists(std::make_pair(LEGACY_DB_COINS, txid_coinbase)));
    }
}

BOOST_AUTO_TEST_SUITE_END()
