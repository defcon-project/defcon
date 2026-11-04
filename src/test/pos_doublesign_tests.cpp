// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <logging.h>
#include <pos/doublesign.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
// Not Hash(): hash.h's Hash<T>(const T&) would win overload resolution for an int.
uint256 BlockId(uint8_t n)
{
    uint256 id;
    *id.begin() = n;
    return id;
}

COutPoint Kernel(uint8_t n) { return COutPoint{BlockId(n), 1}; }

COutPoint KernelNo(size_t i)
{
    uint256 txid;
    std::copy_n(reinterpret_cast<const unsigned char*>(&i), sizeof(i), txid.begin());
    return COutPoint{txid, 0};
}

//! The three encodings of one secp256k1 key; a signature verifies under each.
enum class Enc { Compressed, Uncompressed, Hybrid };

//! Key number n (n >= 1; the secret is n itself) in the encoding asked for.
std::vector<unsigned char> KeyBytes(uint8_t n, Enc enc = Enc::Compressed)
{
    static std::map<std::pair<uint8_t, Enc>, std::vector<unsigned char>> made;
    auto [it, fresh] = made.try_emplace({n, enc});
    if (fresh) {
        unsigned char secret[32]{};
        secret[31] = n;
        CKey key;
        key.Set(std::begin(secret), std::end(secret), /*fCompressedIn=*/enc == Enc::Compressed);
        assert(key.IsValid());
        const CPubKey pub{key.GetPubKey()};
        it->second.assign(pub.begin(), pub.end());
        if (enc == Enc::Hybrid) it->second[0] = 0x06 | (it->second.back() & 1);
    }
    return it->second;
}

//! The script a coinstake's second output carries: pay-to-pubkey.
CScript Signer(uint8_t n, Enc enc = Enc::Compressed) { return CScript() << KeyBytes(n, enc) << OP_CHECKSIG; }
//! A kernel output paying the same key by its hash, as most coins do.
CScript OwnedByHash(uint8_t n, Enc enc = Enc::Compressed) { return GetScriptForDestination(PKHash(CPubKey(KeyBytes(n, enc)))); }

constexpr Enc ENCODINGS[]{Enc::Compressed, Enc::Uncompressed, Enc::Hybrid};

const std::optional<CScript> SPENT{std::nullopt};
constexpr int TIP{5000};

//! A proof-of-stake shaped block whose coinstake spends `kernel` and pays `signer`.
CBlock StakedBlock(const COutPoint& kernel, const CScript& signer)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.resize(1);
    coinbase.vout[0].SetEmpty();
    CMutableTransaction coinstake;
    coinstake.vin.emplace_back(kernel);
    coinstake.vout.resize(2);
    coinstake.vout[0].SetEmpty();
    coinstake.vout[1] = CTxOut(COIN, signer);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    block.vtx.push_back(MakeTransactionRef(coinstake));
    return block;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(pos_doublesign_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(the_owner_is_the_key_the_kernel_pays)
{
    BOOST_CHECK(StakeDoubleSignMonitor::SignerOwnsKernel(Signer(1), Signer(1)));
    BOOST_CHECK(StakeDoubleSignMonitor::SignerOwnsKernel(Signer(1), OwnedByHash(1)));
    BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(Signer(2), Signer(1)));
    BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(Signer(2), OwnedByHash(1)));
    // A signer is a pay-to-pubkey script; a kernel of any other shape has no owner key.
    BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(OwnedByHash(1), OwnedByHash(1)));
    BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(Signer(1), CScript() << OP_TRUE));
}

BOOST_AUTO_TEST_CASE(the_owner_is_the_key_whatever_its_encoding)
{
    // A kernel may pay one encoding of a key, or its hash, and the block be
    // signed with another: it is the same key, so the same owner.
    for (const Enc signer : ENCODINGS) {
        for (const Enc kernel : ENCODINGS) {
            BOOST_CHECK(StakeDoubleSignMonitor::SignerOwnsKernel(Signer(1, signer), Signer(1, kernel)));
            BOOST_CHECK(StakeDoubleSignMonitor::SignerOwnsKernel(Signer(1, signer), OwnedByHash(1, kernel)));
            // Another key is never the owner, in whatever encoding.
            BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(Signer(2, signer), Signer(1, kernel)));
            BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(Signer(2, signer), OwnedByHash(1, kernel)));
        }
    }
    // Bytes shaped like a key but on no curve point are nobody's key, not even their own.
    std::vector<unsigned char> no_point(CPubKey::COMPRESSED_SIZE, 0xff);
    no_point[0] = 0x02;
    const CScript no_key = CScript() << no_point << OP_CHECKSIG;
    BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(no_key, no_key));
    BOOST_CHECK(!StakeDoubleSignMonitor::SignerOwnsKernel(Signer(1), no_key));
}

BOOST_AUTO_TEST_CASE(a_double_signature_across_encodings_counts)
{
    StakeDoubleSignMonitor monitor;
    // The kernel pays the uncompressed key; the owner signs one block with the
    // compressed key, the next with the uncompressed one.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1, Enc::Compressed), Signer(1, Enc::Uncompressed)));
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1, Enc::Uncompressed), SPENT));
    // The kernel pays the hash of the uncompressed key; compressed, then hybrid.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(3), Kernel(2), Signer(2, Enc::Compressed), OwnedByHash(2, Enc::Uncompressed)));
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(4), Kernel(2), Signer(2, Enc::Hybrid), SPENT));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 2u);
    // Another key, in the encoding the kernel uses, still neither counts nor takes room.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(5), Kernel(3), Signer(4, Enc::Uncompressed), OwnedByHash(3, Enc::Uncompressed)));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(6), Kernel(3), Signer(4, Enc::Uncompressed), OwnedByHash(3, Enc::Uncompressed)));
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 2u);
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 2u);
}

BOOST_AUTO_TEST_CASE(two_blocks_signed_by_the_kernel_owner_count_once)
{
    StakeDoubleSignMonitor monitor;
    // The usual order: block 1 while its kernel is unspent, block 2 after block 1 spent it.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT));
    const auto summary = monitor.GetSummary();
    BOOST_CHECK_EQUAL(summary.count, 1u);
    BOOST_REQUIRE(summary.last);
    BOOST_CHECK_EQUAL(summary.last->height, TIP + 1);
    BOOST_CHECK(summary.last->kernel == Kernel(1));
    BOOST_CHECK(summary.last->first == BlockId(1));
    BOOST_CHECK(summary.last->second == BlockId(2));
    // The same block again, and a third one, are the same double signature.
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT));
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(3), Kernel(1), Signer(1), SPENT));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 1u);
    // A pay-to-pubkey kernel works the same way.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(4), Kernel(2), Signer(2), Signer(2)));
    BOOST_CHECK(monitor.Observe(TIP, TIP + 1, BlockId(5), Kernel(2), Signer(2), Signer(2)));
}

BOOST_AUTO_TEST_CASE(blocks_signed_by_another_key_never_count_and_take_no_room)
{
    // Anyone can name somebody else's kernel under a key of their own, as often
    // as they like, before or after the real block.
    StakeDoubleSignMonitor monitor;
    for (uint8_t n = 10; n < 13; ++n) {
        BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(n), Kernel(1), Signer(9), OwnedByHash(1)));
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(13), Kernel(1), Signer(9), SPENT));
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(14), Kernel(1), Signer(9), SPENT));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    BOOST_CHECK(!monitor.GetSummary().last);
    // Forgers before both real blocks, which come before either is connected.
    StakeDoubleSignMonitor race;
    for (uint8_t n = 0; n < StakeDoubleSignMonitor::MAX_BLOCKS_PER_KERNEL + 2; ++n) {
        BOOST_CHECK(!race.Observe(TIP, TIP + 1, BlockId(static_cast<uint8_t>(100 + n)), Kernel(1),
                                  Signer(static_cast<uint8_t>(100 + n)), OwnedByHash(1)));
    }
    BOOST_CHECK(!race.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    BOOST_CHECK(race.Observe(TIP, TIP + 1, BlockId(2), Kernel(1), Signer(1), OwnedByHash(1)));
}

BOOST_AUTO_TEST_CASE(an_unreadable_kernel_is_not_tracked)
{
    // The kernel's output cannot be read (spent before this node saw a block on
    // it): nothing is kept, so nothing is counted -- and nothing fills up either.
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), SPENT));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    for (size_t i = 0; i < StakeDoubleSignMonitor::MAX_KERNELS + 50; ++i) {
        monitor.Observe(TIP, TIP + 1, BlockId(9), KernelNo(i), Signer(9), SPENT);
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    // A readable kernel is still tracked and paired.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(3), Kernel(3), Signer(3), OwnedByHash(3)));
    BOOST_CHECK(monitor.Observe(TIP, TIP + 1, BlockId(4), Kernel(3), Signer(3), SPENT));
}

BOOST_AUTO_TEST_CASE(the_window_follows_the_tip)
{
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(1000, 1000, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    // A block far ahead of the tip, as a requested block during sync can be, moves nothing.
    BOOST_CHECK(!monitor.Observe(1000, 1000 + 3 * StakeDoubleSignMonitor::WINDOW, BlockId(7), Kernel(7), Signer(7), OwnedByHash(7)));
    BOOST_CHECK(monitor.Observe(1000, 1000, BlockId(2), Kernel(1), Signer(1), SPENT));
    // Once the tip has moved on, the old height is gone and a late block there is not compared.
    StakeDoubleSignMonitor moved;
    BOOST_CHECK(!moved.Observe(1000, 1000, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    BOOST_CHECK(!moved.Observe(1000 + StakeDoubleSignMonitor::WINDOW + 1, 1000, BlockId(2), Kernel(1), Signer(1), SPENT));
    BOOST_CHECK_EQUAL(moved.GetSummary().count, 0u);
}

BOOST_AUTO_TEST_CASE(the_record_is_bounded_and_keeps_what_it_holds)
{
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(TIP, TIP, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1)));
    // An owner signing one block on each of many outputs of its own fills the record ...
    for (size_t i = 0; i < StakeDoubleSignMonitor::MAX_KERNELS + 50; ++i) {
        monitor.Observe(TIP, TIP + 1, BlockId(9), KernelNo(i), Signer(9), OwnedByHash(9));
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), StakeDoubleSignMonitor::MAX_KERNELS);
    // ... what it holds still pairs ...
    BOOST_CHECK(monitor.Observe(TIP, TIP, BlockId(2), Kernel(1), Signer(1), SPENT));
    // ... and a new pair is not tracked until room frees up as the tip moves on.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 2, BlockId(3), Kernel(3), Signer(3), OwnedByHash(3)));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 2, BlockId(4), Kernel(3), Signer(3), OwnedByHash(3)));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 1u);
    BOOST_CHECK(!monitor.Observe(TIP + 2 + StakeDoubleSignMonitor::WINDOW - 1, TIP + 2, BlockId(5), Kernel(5), Signer(5), OwnedByHash(5)));
    BOOST_CHECK(monitor.Observe(TIP + 2 + StakeDoubleSignMonitor::WINDOW - 1, TIP + 2, BlockId(6), Kernel(5), Signer(5), SPENT));
}

BOOST_AUTO_TEST_CASE(a_failing_read_never_reaches_block_acceptance)
{
    const CBlock block = StakedBlock(Kernel(1), Signer(1));
    const uint64_t before = GetStakeDoubleSignMonitor().GetSummary().count;
    BOOST_CHECK_NO_THROW(ObserveStakeDoubleSign(block, TIP + 1, BlockId(1), TIP,
                                                [](const COutPoint&) -> std::optional<CScript> { throw std::bad_alloc(); }));
    BOOST_CHECK_EQUAL(GetStakeDoubleSignMonitor().GetSummary().count, before);
}

BOOST_AUTO_TEST_CASE(a_failure_to_report_a_failure_never_reaches_block_acceptance)
{
    // Lasting memory shortage: the read fails, and so does the log line that
    // reports it. Something that is no std::exception at all, too.
    const CBlock block = StakedBlock(Kernel(1), Signer(1));
    int reports{0};
    const auto failing_log = LogInstance().PushBackCallback([&](const std::string& line) {
        if (line.find("ObserveStakeDoubleSign") == std::string::npos) return;
        ++reports;
        throw std::bad_alloc();
    });
    const uint64_t before = GetStakeDoubleSignMonitor().GetSummary().count;
    BOOST_CHECK_NO_THROW(ObserveStakeDoubleSign(block, TIP + 1, BlockId(1), TIP,
                                                [](const COutPoint&) -> std::optional<CScript> { throw std::bad_alloc(); }));
    BOOST_CHECK_NO_THROW(ObserveStakeDoubleSign(block, TIP + 1, BlockId(1), TIP,
                                                [](const COutPoint&) -> std::optional<CScript> { throw 42; }));
    LogInstance().DeleteCallback(failing_log);
    // Both failures were reported, and both reports failed in turn.
    BOOST_CHECK_EQUAL(reports, 2);
    BOOST_CHECK_EQUAL(GetStakeDoubleSignMonitor().GetSummary().count, before);
}

BOOST_AUTO_TEST_CASE(only_a_proof_of_stake_block_is_observed)
{
    // A proof-of-work block has no coinstake; nothing is read and nothing counted.
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.resize(1);
    block.vtx.push_back(MakeTransactionRef(coinbase));
    bool read{false};
    const auto reader = [&](const COutPoint&) -> std::optional<CScript> { read = true; return OwnedByHash(1); };
    const uint64_t before = GetStakeDoubleSignMonitor().GetSummary().count;
    ObserveStakeDoubleSign(block, 10, BlockId(1), 10, reader);
    ObserveStakeDoubleSign(block, 10, BlockId(2), 10, reader);
    BOOST_CHECK(!read);
    BOOST_CHECK_EQUAL(GetStakeDoubleSignMonitor().GetSummary().count, before);
}

BOOST_AUTO_TEST_SUITE_END()
