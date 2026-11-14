// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <key.h>
#include <logging.h>
#include <pos/doublesign.h>
#include <pos/kernel.h>
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
using Verdict = StakeDoubleSignMonitor::StakeVerdict;
//! A stake check every kernel passes: most cases below are about owners and pairs.
const StakeDoubleSignMonitor::StakeCheck ANY{[](const COutPoint&, const StakeDoubleSignMonitor::KernelCoin&) { return Verdict::QUALIFIES; }};
//! One no kernel passes: the owner signing on outputs that could not stake the block.
const StakeDoubleSignMonitor::StakeCheck NONE{[](const COutPoint&, const StakeDoubleSignMonitor::KernelCoin&) { return Verdict::FAILS; }};
//! One that cannot decide: the block's parent, or its kernel's funding block, is off the active chain.
const StakeDoubleSignMonitor::StakeCheck UNDECIDABLE{[](const COutPoint&, const StakeDoubleSignMonitor::KernelCoin&) { return Verdict::UNDECIDED; }};
//! No settled parent: ObserveStakeDoubleSign's cases below never reach the judgement.
const StakeDoubleSignMonitor::StakeContext NO_CONTEXT{};
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
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1, Enc::Compressed), Signer(1, Enc::Uncompressed), ANY));
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1, Enc::Uncompressed), SPENT, ANY));
    // The kernel pays the hash of the uncompressed key; compressed, then hybrid.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(3), Kernel(2), Signer(2, Enc::Compressed), OwnedByHash(2, Enc::Uncompressed), ANY));
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(4), Kernel(2), Signer(2, Enc::Hybrid), SPENT, ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 2u);
    // Another key, in the encoding the kernel uses, still neither counts nor takes room.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(5), Kernel(3), Signer(4, Enc::Uncompressed), OwnedByHash(3, Enc::Uncompressed), ANY));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(6), Kernel(3), Signer(4, Enc::Uncompressed), OwnedByHash(3, Enc::Uncompressed), ANY));
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 2u);
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 2u);
}

BOOST_AUTO_TEST_CASE(two_blocks_signed_by_the_kernel_owner_count_once)
{
    StakeDoubleSignMonitor monitor;
    // The usual order: block 1 while its kernel is unspent, block 2 after block 1 spent it.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT, ANY));
    const auto summary = monitor.GetSummary();
    BOOST_CHECK_EQUAL(summary.count, 1u);
    BOOST_REQUIRE(summary.last);
    BOOST_CHECK_EQUAL(summary.last->height, TIP + 1);
    BOOST_CHECK(summary.last->kernel == Kernel(1));
    BOOST_CHECK(summary.last->first == BlockId(1));
    BOOST_CHECK(summary.last->second == BlockId(2));
    // The same block again, and a third one, are the same double signature.
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT, ANY));
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(3), Kernel(1), Signer(1), SPENT, ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 1u);
    // A pay-to-pubkey kernel works the same way.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(4), Kernel(2), Signer(2), Signer(2), ANY));
    BOOST_CHECK(monitor.Observe(TIP, TIP + 1, BlockId(5), Kernel(2), Signer(2), Signer(2), ANY));
}

BOOST_AUTO_TEST_CASE(blocks_signed_by_another_key_never_count_and_take_no_room)
{
    // Anyone can name somebody else's kernel under a key of their own, as often
    // as they like, before or after the real block.
    StakeDoubleSignMonitor monitor;
    for (uint8_t n = 10; n < 13; ++n) {
        BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(n), Kernel(1), Signer(9), OwnedByHash(1), ANY));
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(13), Kernel(1), Signer(9), SPENT, ANY));
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(14), Kernel(1), Signer(9), SPENT, ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    BOOST_CHECK(!monitor.GetSummary().last);
    // Forgers before both real blocks, which come before either is connected.
    StakeDoubleSignMonitor race;
    for (uint8_t n = 0; n < StakeDoubleSignMonitor::MAX_BLOCKS_PER_KERNEL + 2; ++n) {
        BOOST_CHECK(!race.Observe(TIP, TIP + 1, BlockId(static_cast<uint8_t>(100 + n)), Kernel(1),
                                  Signer(static_cast<uint8_t>(100 + n)), OwnedByHash(1), ANY));
    }
    BOOST_CHECK(!race.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    BOOST_CHECK(race.Observe(TIP, TIP + 1, BlockId(2), Kernel(1), Signer(1), OwnedByHash(1), ANY));
}

BOOST_AUTO_TEST_CASE(a_kernel_that_could_not_stake_never_counts_and_takes_no_room)
{
    // The owner signs two blocks on an output of their own that could stake neither: blocks
    // are observed before their kernel is checked, so this is anyone with a coin.
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), NONE));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(2), Kernel(1), Signer(1), OwnedByHash(1), NONE));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    // ... on as many outputs as they have: the record does not fill up ...
    for (size_t i = 0; i < StakeDoubleSignMonitor::MAX_KERNELS + 50; ++i) {
        monitor.Observe(TIP, TIP + 1, BlockId(9), KernelNo(i), Signer(9), OwnedByHash(9), NONE);
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    // A block that cannot be judged here is not kept either.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(5), Kernel(5), Signer(5), OwnedByHash(5), UNDECIDABLE));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(6), Kernel(5), Signer(5), OwnedByHash(5), UNDECIDABLE));
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    // ... and a double signature on kernels that could stake is still counted.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(3), Kernel(3), Signer(3), OwnedByHash(3), ANY));
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(4), Kernel(3), Signer(3), SPENT, ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 1u);
}

BOOST_AUTO_TEST_CASE(every_block_is_judged_by_the_output_its_first_block_found)
{
    // The second block usually comes once the first has spent the kernel; it is judged by the
    // output recorded with the first, and only a block that could stake on it pairs.
    StakeDoubleSignMonitor monitor;
    const StakeDoubleSignMonitor::KernelCoin coin{OwnedByHash(1), 7 * COIN, 1234, BlockId(77)};
    std::vector<StakeDoubleSignMonitor::KernelCoin> judged;
    bool could{true};
    const StakeDoubleSignMonitor::StakeCheck check{[&](const COutPoint& kernel, const StakeDoubleSignMonitor::KernelCoin& c) {
        BOOST_CHECK(kernel == Kernel(1));
        judged.push_back(c);
        return could ? Verdict::QUALIFIES : Verdict::FAILS;
    }};
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), coin, check));
    // a second block that could not stake on it (at another time, say): kept nowhere
    could = false;
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT, check));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    // a third that could: the pair is the first and the third
    could = true;
    BOOST_CHECK(monitor.Observe(TIP + 1, TIP + 1, BlockId(3), Kernel(1), Signer(1), SPENT, check));
    BOOST_REQUIRE_EQUAL(judged.size(), 3u);
    for (const auto& c : judged) {
        BOOST_CHECK(c.script == coin.script);
        BOOST_CHECK_EQUAL(c.value, coin.value);
        BOOST_CHECK_EQUAL(c.height, coin.height);
        BOOST_CHECK(c.funding == coin.funding);
    }
    const auto summary = monitor.GetSummary();
    BOOST_REQUIRE(summary.last);
    BOOST_CHECK(summary.last->first == BlockId(1));
    BOOST_CHECK(summary.last->second == BlockId(3));
    // An output read afresh is what a held pair's next block is judged by.
    judged.clear();
    const StakeDoubleSignMonitor::KernelCoin fresh{OwnedByHash(1), 7 * COIN, 1200, BlockId(78)};
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(4), Kernel(1), Signer(1), fresh, check));
    BOOST_REQUIRE_EQUAL(judged.size(), 1u);
    BOOST_CHECK(judged[0].funding == fresh.funding);
    BOOST_CHECK_EQUAL(judged[0].height, fresh.height);
    // Another key is turned away before the check is asked.
    judged.clear();
    BOOST_CHECK(!monitor.Observe(TIP + 1, TIP + 1, BlockId(5), Kernel(1), Signer(9), SPENT, check));
    BOOST_CHECK(judged.empty());
}

namespace {
int64_t g_rule_from_time{0};
//! A kernel rule that records the funding block time it is given and accepts.
bool RecordingRule(const CBlockIndex*, const Consensus::Params&, const COutPoint&, CAmount, int, int64_t from_time, int64_t,
                   unsigned int)
{
    g_rule_from_time = from_time;
    return true;
}
} // namespace

BOOST_AUTO_TEST_CASE(a_block_is_judged_on_its_own_branch_only)
{
    // Block indexes at heights 0..1000, one minute apart.
    constexpr int TOP{1000};
    std::vector<uint256> hashes(TOP + 1);
    std::vector<CBlockIndex> chain(TOP + 1);
    for (int i = 0; i <= TOP; ++i) {
        hashes[i] = ArithToUint256(arith_uint256(static_cast<uint64_t>(i) + 1));
        chain[i].phashBlock = &hashes[i];
        chain[i].nHeight = i;
        chain[i].nTime = 1700000000 + 60 * i;
        chain[i].pprev = i > 0 ? &chain[i - 1] : nullptr;
        chain[i].BuildSkip();
    }
    Consensus::Params params = Params().GetConsensus();
    params.stakeValueRange = {1 * COIN, 1000 * COIN};
    params.stakeAgeRange = {10 * 60, 60 * 60 * 24};
    params.nPosKernelV2ActivationHeight = 0;
    const COutPoint kernel{BlockId(1), 1};
    const int deep_enough{TOP - (COINBASE_MATURITY + 1)};
    // Under the corrected kernel every hash meets the easiest target.
    StakeDoubleSignMonitor::StakeContext context{&chain[TOP], chain[TOP].nTime + 60, 0x207fffff, &params, &StakeKernelQualifies};
    const StakeDoubleSignMonitor::KernelCoin coin{OwnedByHash(1), 10 * COIN, deep_enough, hashes[deep_enough]};
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(context, kernel, coin) == Verdict::QUALIFIES);

    // A parent that is not settled: not judged.
    StakeDoubleSignMonitor::StakeContext unsettled{context};
    unsettled.settled_parent = nullptr;
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(unsettled, kernel, coin) == Verdict::UNDECIDED);
    // The output recorded with its funding block on another branch -- at the same height, or
    // deeper than it is on this one: its height and time are not this branch's, so not judged.
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(context, kernel, {OwnedByHash(1), 10 * COIN, deep_enough, BlockId(200)}) == Verdict::UNDECIDED);
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(context, kernel, {OwnedByHash(1), 10 * COIN, deep_enough - 40, hashes[deep_enough]}) == Verdict::UNDECIDED);
    // On this branch: too young, created above the parent, out of the value range, an unreachable target.
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(context, kernel, {OwnedByHash(1), 10 * COIN, deep_enough + 1, hashes[deep_enough + 1]}) == Verdict::FAILS);
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(context, kernel, {OwnedByHash(1), 10 * COIN, TOP + 1, BlockId(201)}) == Verdict::FAILS);
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(context, kernel, {OwnedByHash(1), 2000 * COIN, deep_enough, hashes[deep_enough]}) == Verdict::FAILS);
    StakeDoubleSignMonitor::StakeContext hard{context};
    hard.bits = 0x03000001;
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(hard, kernel, coin) == Verdict::FAILS);
    // The rule gets the funding block's time from this branch's ancestor.
    StakeDoubleSignMonitor::StakeContext recording{context};
    recording.rule = &RecordingRule;
    BOOST_CHECK(StakeDoubleSignMonitor::Judge(recording, kernel, coin) == Verdict::QUALIFIES);
    BOOST_CHECK_EQUAL(g_rule_from_time, chain[deep_enough].GetBlockTime());

    // Through block acceptance's entry point: a staked block on an unsettled parent is not kept.
    const CBlock block = StakedBlock(kernel, Signer(1));
    const auto before = GetStakeDoubleSignMonitor().GetSummary().count;
    const auto tracked = GetStakeDoubleSignMonitor().KernelsTracked();
    for (uint8_t n = 1; n <= 2; ++n) {
        BOOST_CHECK_NO_THROW(ObserveStakeDoubleSign(block, TIP + 1, BlockId(static_cast<uint8_t>(30 + n)), TIP,
                                                    [&](const COutPoint&) -> std::optional<StakeDoubleSignMonitor::KernelCoin> { return coin; },
                                                    unsettled));
    }
    BOOST_CHECK_EQUAL(GetStakeDoubleSignMonitor().GetSummary().count, before);
    BOOST_CHECK_EQUAL(GetStakeDoubleSignMonitor().KernelsTracked(), tracked);
}

BOOST_AUTO_TEST_CASE(an_unreadable_kernel_is_not_tracked)
{
    // The kernel's output cannot be read (spent before this node saw a block on
    // it): nothing is kept, so nothing is counted -- and nothing fills up either.
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(1), Kernel(1), Signer(1), SPENT, ANY));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(2), Kernel(1), Signer(1), SPENT, ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 0u);
    for (size_t i = 0; i < StakeDoubleSignMonitor::MAX_KERNELS + 50; ++i) {
        monitor.Observe(TIP, TIP + 1, BlockId(9), KernelNo(i), Signer(9), SPENT, ANY);
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), 0u);
    // A readable kernel is still tracked and paired.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 1, BlockId(3), Kernel(3), Signer(3), OwnedByHash(3), ANY));
    BOOST_CHECK(monitor.Observe(TIP, TIP + 1, BlockId(4), Kernel(3), Signer(3), SPENT, ANY));
}

BOOST_AUTO_TEST_CASE(the_window_follows_the_tip)
{
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(1000, 1000, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    // A block far ahead of the tip, as a requested block during sync can be, moves nothing.
    BOOST_CHECK(!monitor.Observe(1000, 1000 + 3 * StakeDoubleSignMonitor::WINDOW, BlockId(7), Kernel(7), Signer(7), OwnedByHash(7), ANY));
    BOOST_CHECK(monitor.Observe(1000, 1000, BlockId(2), Kernel(1), Signer(1), SPENT, ANY));
    // Once the tip has moved on, the old height is gone and a late block there is not compared.
    StakeDoubleSignMonitor moved;
    BOOST_CHECK(!moved.Observe(1000, 1000, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    BOOST_CHECK(!moved.Observe(1000 + StakeDoubleSignMonitor::WINDOW + 1, 1000, BlockId(2), Kernel(1), Signer(1), SPENT, ANY));
    BOOST_CHECK_EQUAL(moved.GetSummary().count, 0u);
}

BOOST_AUTO_TEST_CASE(the_record_is_bounded_and_keeps_what_it_holds)
{
    StakeDoubleSignMonitor monitor;
    BOOST_CHECK(!monitor.Observe(TIP, TIP, BlockId(1), Kernel(1), Signer(1), OwnedByHash(1), ANY));
    // An owner signing one block on each of many outputs of its own fills the record ...
    for (size_t i = 0; i < StakeDoubleSignMonitor::MAX_KERNELS + 50; ++i) {
        monitor.Observe(TIP, TIP + 1, BlockId(9), KernelNo(i), Signer(9), OwnedByHash(9), ANY);
    }
    BOOST_CHECK_EQUAL(monitor.KernelsTracked(), StakeDoubleSignMonitor::MAX_KERNELS);
    // ... what it holds still pairs ...
    BOOST_CHECK(monitor.Observe(TIP, TIP, BlockId(2), Kernel(1), Signer(1), SPENT, ANY));
    // ... and a new pair is not tracked until room frees up as the tip moves on.
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 2, BlockId(3), Kernel(3), Signer(3), OwnedByHash(3), ANY));
    BOOST_CHECK(!monitor.Observe(TIP, TIP + 2, BlockId(4), Kernel(3), Signer(3), OwnedByHash(3), ANY));
    BOOST_CHECK_EQUAL(monitor.GetSummary().count, 1u);
    BOOST_CHECK(!monitor.Observe(TIP + 2 + StakeDoubleSignMonitor::WINDOW - 1, TIP + 2, BlockId(5), Kernel(5), Signer(5), OwnedByHash(5), ANY));
    BOOST_CHECK(monitor.Observe(TIP + 2 + StakeDoubleSignMonitor::WINDOW - 1, TIP + 2, BlockId(6), Kernel(5), Signer(5), SPENT, ANY));
}

BOOST_AUTO_TEST_CASE(a_failing_read_never_reaches_block_acceptance)
{
    const CBlock block = StakedBlock(Kernel(1), Signer(1));
    const uint64_t before = GetStakeDoubleSignMonitor().GetSummary().count;
    BOOST_CHECK_NO_THROW(ObserveStakeDoubleSign(block, TIP + 1, BlockId(1), TIP,
                                                [](const COutPoint&) -> std::optional<CScript> { throw std::bad_alloc(); }, NO_CONTEXT));
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
                                                [](const COutPoint&) -> std::optional<CScript> { throw std::bad_alloc(); }, NO_CONTEXT));
    BOOST_CHECK_NO_THROW(ObserveStakeDoubleSign(block, TIP + 1, BlockId(1), TIP,
                                                [](const COutPoint&) -> std::optional<CScript> { throw 42; }, NO_CONTEXT));
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
    ObserveStakeDoubleSign(block, 10, BlockId(1), 10, reader, NO_CONTEXT);
    ObserveStakeDoubleSign(block, 10, BlockId(2), 10, reader, NO_CONTEXT);
    BOOST_CHECK(!read);
    BOOST_CHECK_EQUAL(GetStakeDoubleSignMonitor().GetSummary().count, before);
}

BOOST_AUTO_TEST_SUITE_END()
