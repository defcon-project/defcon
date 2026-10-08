// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <clientversion.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

// Regression coverage for F-2026-118: the undo format carries a fork-added
// fCoinStake bit (undo.h, TxInUndoFormatter: nHeight*4 + fCoinBase +
// fCoinStake*2), and it is the record DisconnectBlock reads to restore a spent
// input during a reorg. Nothing exercised the round trip: coins_tests covers
// the Coin chainstate format (a different encoding, nHeight*2 + fCoinBase plus
// a separate flag byte), and the txundo fuzz target only deserializes. A
// coinstake could have been serialized here and read back as an ordinary coin
// with the suite still green, and on a reorg that would make a restored
// coinstake spendable before maturity on the nodes that reorged but not on
// those that did not.
BOOST_FIXTURE_TEST_SUITE(undo_tests, BasicTestingSetup)

namespace {
Coin RoundTripThroughUndo(const Coin& in)
{
    CTxUndo undo;
    undo.vprevout.push_back(in);
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << undo;
    CTxUndo out;
    ss >> out;
    BOOST_REQUIRE_EQUAL(out.vprevout.size(), 1U);
    return out.vprevout[0];
}
} // namespace

BOOST_AUTO_TEST_CASE(coinstake_flag_survives_the_undo_round_trip)
{
    const CScript script = CScript() << OP_TRUE;

    // A coinstake output: not a coinbase, coinstake bit set.
    const Coin coinstake(CTxOut(CAmount{1234}, script), /*nHeightIn=*/7,
                         /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/true);
    const Coin back = RoundTripThroughUndo(coinstake);
    BOOST_CHECK_EQUAL(back.IsCoinStake(), true);
    BOOST_CHECK_EQUAL(back.IsCoinBase(), false);
    BOOST_CHECK_EQUAL(back.nHeight, 7U);
    BOOST_CHECK_EQUAL(back.out.nValue, CAmount{1234});
    BOOST_CHECK(back.out.scriptPubKey == script);

    // A coinbase output: coinbase bit set, coinstake clear. The two bits are
    // independent and must not be confused for one another.
    const Coin coinbase(CTxOut(CAmount{1234}, script), 7,
                        /*fCoinBaseIn=*/true, /*fCoinStakeIn=*/false);
    const Coin cbBack = RoundTripThroughUndo(coinbase);
    BOOST_CHECK_EQUAL(cbBack.IsCoinBase(), true);
    BOOST_CHECK_EQUAL(cbBack.IsCoinStake(), false);

    // A plain output: neither bit.
    const Coin plain(CTxOut(CAmount{1234}, script), 7, false, false);
    const Coin plainBack = RoundTripThroughUndo(plain);
    BOOST_CHECK_EQUAL(plainBack.IsCoinBase(), false);
    BOOST_CHECK_EQUAL(plainBack.IsCoinStake(), false);
}

// The flag is actually written into the undo bytes, not reconstructed from
// context. TxInUndoFormatter::Ser writes VARINT(nHeight*4 + fCoinBase +
// fCoinStake*2) first. For a non-coinbase output at height 7 the code is 28
// (0x1c) without the flag and 30 (0x1e) with it, both single-byte VARINTs, so
// flipping only fCoinStake changes exactly that one byte by 2. CTxUndo prefixes
// a compactsize count of 1 (0x01), so the code byte is the second byte.
// If the "+ fCoinStake*2" term were dropped from the formatter, the coinstake
// coin would serialize with 0x1c here and this case would fail -- which is the
// negative control this regression exists to be.
BOOST_AUTO_TEST_CASE(coinstake_bit_is_present_in_the_serialized_undo)
{
    const CScript script = CScript() << OP_TRUE;
    auto hex_of = [&](bool coinstake) {
        CTxUndo undo;
        undo.vprevout.emplace_back(CTxOut(CAmount{1234}, script), /*nHeightIn=*/7,
                                   /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/coinstake);
        CDataStream ss(SER_DISK, CLIENT_VERSION);
        ss << undo;
        return HexStr(ss);
    };
    const std::string with = hex_of(true);
    const std::string without = hex_of(false);
    BOOST_REQUIRE_GE(with.size(), 4U);
    BOOST_CHECK_EQUAL(with.substr(0, 2), "01");   // compactsize count of one prevout
    BOOST_CHECK_EQUAL(with.substr(2, 2), "1e");    // 7*4 + 0 + 2
    BOOST_CHECK_EQUAL(without.substr(2, 2), "1c"); // 7*4 + 0 + 0
    // Nothing else differs: the dummy zero byte and the compressed txout that
    // follow the code are identical, so the two records differ in exactly the
    // one bit under test.
    BOOST_CHECK_EQUAL(with.substr(4), without.substr(4));
}

BOOST_AUTO_TEST_SUITE_END()
