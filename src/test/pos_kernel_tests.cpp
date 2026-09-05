// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/validation.h>
#include <pos/kernel.h>
#include <primitives/transaction.h>
#include <util/system.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pos_kernel_tests, TestChain100Setup)

namespace {
//! A transaction shaped like a coinstake (empty first output) spending `prevout`.
CMutableTransaction MakeCoinstakeSpending(const COutPoint& prevout)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(prevout);
    tx.vout.emplace_back(CTxOut());          // vout[0] empty: the coinstake marker
    tx.vout[0].SetEmpty();
    tx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    BOOST_REQUIRE(CTransaction(tx).IsCoinStake());
    return tx;
}
} // namespace

/**
 * Every rejection inside CheckProofOfStake must leave the state invalid.
 *
 * ConnectTip only calls InvalidBlockFound() when state.IsInvalid(); a rejection that
 * returns bare `false` therefore leaves the block unmarked, still a candidate in the
 * block index, and the next ActivateBestChain tries it again. Five of the eight
 * rejection paths used to do exactly that -- this test pins the contract for the two
 * that are reachable without a full staking setup, plus two that always set it.
 */
BOOST_AUTO_TEST_CASE(rejections_set_the_validation_state)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);
    BOOST_REQUIRE_EQUAL(tip->nHeight, 100);

    const unsigned int nBits = tip->nBits;
    uint256 hashProof, target;

    // A transaction that is not a coinstake at all: always set the state.
    {
        CMutableTransaction plain;
        plain.vin.emplace_back(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
        plain.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(plain), tip->nTime, nBits,
                                       hashProof, target));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "malformed-txn");
    }

    // An input that no coin backs: always set the state.
    {
        const auto tx = MakeCoinstakeSpending(COutPoint(uint256::ONE, 0));
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), tip->nTime, nBits,
                                       hashProof, target));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "prevout-not-found");
    }

    // Immature stake: the newest coinbase is 0 deep, well under COINBASE_MATURITY + 1.
    // This branch used to return a bare false and leave the block unmarked.
    {
        const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns.back()->GetHash(), 0));
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), tip->nTime, nBits,
                                       hashProof, target));
        BOOST_CHECK_MESSAGE(state.IsInvalid(),
                            "an immature stake must mark the block invalid, or ConnectTip retries it forever");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-depth");
    }

    // Mature coin, but the stake time equals the funding block time, so the input age is
    // 0 -- below stakeAgeRange[0]. Also a bare false before this change.
    {
        const CBlockIndex* funding = chainstate.m_chain[1];
        BOOST_REQUIRE(funding != nullptr);
        const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
        BlockValidationState state;
        BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), funding->GetBlockTime(),
                                       nBits, hashProof, target));
        BOOST_CHECK_MESSAGE(state.IsInvalid(), "an out-of-range stake age must mark the block invalid");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-age");
    }
}

/**
 * A larger stake must never be less likely to win than a smaller one.
 *
 * The kernel hash does not depend on the amount -- only the stake modifier, the
 * outpoint and the two timestamps feed it -- so the amount moves the threshold
 * and nothing else. Weighting the target by multiplying it therefore has a
 * clean failure mode: the product exceeds 256 bits, arith_uint256 keeps the low
 * bits and drops the rest without a word, and the threshold lands somewhere
 * arbitrary. Two stakes that differ only in size then get thresholds in no
 * particular order.
 *
 * That happens exactly where the untruncated product would exceed every
 * possible hash, which is to say where the check should accept everything. At
 * the difficulty floor -- posLimit, which is where a chain sits when block
 * production has just started or has stalled -- it is reached by any stake of a
 * few dozen coins.
 */
BOOST_AUTO_TEST_CASE(a_larger_stake_is_never_ranked_below_a_smaller_one)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    Consensus::Params corrected = Params().GetConsensus();
    corrected.nPosKernelV2ActivationHeight = 0;
    Consensus::Params original = corrected;
    original.nPosKernelV2ActivationHeight = std::numeric_limits<int>::max();

    const unsigned int nBits = UintToArith256(corrected.posLimit).GetCompact();
    const COutPoint prevout(m_coinbase_txns[0]->GetHash(), 0);
    const CBlockIndex* funding = chainstate.m_chain[1];
    BOOST_REQUIRE(funding != nullptr);
    const uint32_t nBlockFromTime = static_cast<uint32_t>(funding->GetBlockTime());
    const uint32_t nTime = nBlockFromTime + 3600;

    const auto accepts = [&](const Consensus::Params& params, CAmount amount) {
        uint256 hashProof, target;
        return CheckStakeKernelHash(tip, params, nBits, nBlockFromTime, amount, prevout, nTime,
                                    hashProof, target);
    };

    std::vector<CAmount> amounts;
    for (int i = 1; i <= 400; ++i) {
        amounts.push_back(static_cast<CAmount>(i) * 5 * COIN);
    }

    // The corrected rules divide the hash by the weight, so a bigger weight can
    // only ever help: once a stake is large enough to be accepted, every larger
    // one must be too.
    bool accepted_something = false;
    for (const CAmount amount : amounts) {
        const bool ok = accepts(corrected, amount);
        if (accepted_something) {
            BOOST_CHECK_MESSAGE(ok, "a stake of " << amount / COIN
                                    << " was rejected while a smaller one was accepted");
        }
        accepted_something = accepted_something || ok;
    }
    BOOST_CHECK_MESSAGE(accepted_something, "no stake in the range was accepted at all");

    // The original rules do not hold that line. Find the inversion rather than
    // hard-coding one, so this states the property and not a fixture detail.
    CAmount smaller_accepted = 0, larger_rejected = 0;
    for (const CAmount amount : amounts) {
        if (accepts(original, amount)) {
            smaller_accepted = amount;
        } else if (smaller_accepted != 0) {
            larger_rejected = amount;
            break;
        }
    }
    BOOST_CHECK_MESSAGE(larger_rejected != 0,
                        "expected the original rules to rank some larger stake below a smaller one");
    BOOST_TEST_MESSAGE("original rules accepted a stake of " << smaller_accepted / COIN
                       << " and rejected the larger " << larger_rejected / COIN);
}

/**
 * Age alone must not retire a coin.
 *
 * A coin becomes ineligible once it is older than stakeAgeRange[1], and the
 * only thing that refreshes it is winning a block -- which is the one thing a
 * coin that never wins cannot do. Nothing in this kernel rewards age, so the
 * bound moderated no advantage; it simply removed coins from the set, silently
 * and for good.
 *
 * Under the corrected rules an over-age coin gets past the age check and is
 * judged on the same terms as any other. It still fails here, because this
 * coinstake carries no signature -- but it fails for that reason, which is the
 * point.
 */
BOOST_AUTO_TEST_CASE(an_old_coin_may_still_stake)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE_MESSAGE(IsPosKernelV2(params, tip->nHeight + 1),
                          "this test needs the corrected rules active on regtest");

    const CBlockIndex* funding = chainstate.m_chain[1];
    BOOST_REQUIRE(funding != nullptr);
    const int64_t past_the_cap = funding->GetBlockTime() + params.stakeAgeRange[1] + 1;

    const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
    BlockValidationState state;
    uint256 hashProof, target;
    BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), past_the_cap,
                                   tip->nBits, hashProof, target));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK_MESSAGE(state.GetRejectReason() != "bad-stake-age",
                        "an over-age coin was still turned away for its age");
}

/**
 * The same coin, under the rules the change replaced.
 *
 * an_old_coin_may_still_stake shows the corrected kernel letting an over-age
 * coin through. That only means something if the original kernel turned the
 * same coin away for the same reason, so this runs the identical fixture with
 * the activation height pushed out of reach. The age check sits ahead of the
 * signature check in CheckProofOfStake, which is why an unsigned coinstake is
 * enough to read the verdict: under these rules it never gets far enough to
 * fail for anything else.
 */
struct PosKernelOriginalRulesSetup : public TestChain100Setup {
    PosKernelOriginalRulesSetup() : TestChain100Setup{{"-testactivationheight=posv2@1000000"}} {}
};

BOOST_FIXTURE_TEST_CASE(an_old_coin_is_retired_under_the_original_rules, PosKernelOriginalRulesSetup)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip != nullptr);

    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE_MESSAGE(!IsPosKernelV2(params, tip->nHeight + 1),
                          "this test needs the original rules, so the override must have applied");

    const CBlockIndex* funding = chainstate.m_chain[1];
    BOOST_REQUIRE(funding != nullptr);
    const int64_t past_the_cap = funding->GetBlockTime() + params.stakeAgeRange[1] + 1;

    const auto tx = MakeCoinstakeSpending(COutPoint(m_coinbase_txns[0]->GetHash(), 0));
    BlockValidationState state;
    uint256 hashProof, target;
    BOOST_CHECK(!CheckProofOfStake(chainstate, state, tip, CTransaction(tx), past_the_cap,
                                   tip->nBits, hashProof, target));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-age");
}

/**
 * The activation height is the rollout, so it belongs in a test.
 *
 * Both halves matter and they fail in opposite directions. A devnet height
 * that moves invalidates every measurement taken across it -- the chain says
 * one rule applied where the record says another. A mainnet or testnet height
 * that appears by accident is a consensus change nobody decided to make, on a
 * network that is still running the original kernel deliberately.
 */
// The nonce rule is a pure function of height, nonce and params, and is tested
// as one because no unit fixture reaches a proof-of-stake height on regtest
// (lastPowBlock is 5000 there and TestChain100Setup mines 100 blocks). Every
// combination that matters is enumerated: below and above lastPowBlock, nonce
// zero and not, gate reached and not.
BOOST_AUTO_TEST_CASE(pos_block_nonce_rule)
{
    Consensus::Params p;
    p.lastPowBlock = 999;

    // Proof-of-work heights: any nonce, including zero, at every height up to
    // and including the boundary.
    BOOST_CHECK(CheckPosBlockNonce(1, 0, p));
    BOOST_CHECK(CheckPosBlockNonce(500, 0, p));
    BOOST_CHECK(CheckPosBlockNonce(500, 12345, p));
    BOOST_CHECK(CheckPosBlockNonce(999, 1, p));
    BOOST_CHECK(CheckPosBlockNonce(999, 12345, p));

    // The first proof-of-stake height and every height after it: nonce zero
    // only. There is no height below which this is waived -- see the header for
    // why leaving it optional is the dangerous choice.
    BOOST_CHECK(CheckPosBlockNonce(1000, 0, p));
    BOOST_CHECK(!CheckPosBlockNonce(1000, 1, p));
    BOOST_CHECK(!CheckPosBlockNonce(1000, 12345, p));
    BOOST_CHECK(CheckPosBlockNonce(7559, 0, p));
    BOOST_CHECK(!CheckPosBlockNonce(7559, 12345, p));
    BOOST_CHECK(CheckPosBlockNonce(100000, 0, p));
    BOOST_CHECK(!CheckPosBlockNonce(100000, 1, p));

    // The rule follows lastPowBlock, so a network with a different boundary
    // gets the same shape at its own boundary.
    Consensus::Params q;
    q.lastPowBlock = 5000;
    BOOST_CHECK(CheckPosBlockNonce(5000, 42, q));
    BOOST_CHECK(!CheckPosBlockNonce(5001, 42, q));
    BOOST_CHECK(CheckPosBlockNonce(5001, 0, q));
}

// The shape of the hazard this rule exists for, pinned as arithmetic so the two
// readings of a nonce cannot drift apart again.
//
// A block says it is proof of stake by carrying a coinstake; an index entry says
// it by carrying nonce zero. Header acceptance requires proof of work by height,
// so nothing stops a header at a proof-of-stake height carrying a non-zero nonce
// -- and once stored, the index calls it proof of work and LoadBlockIndexGuts
// re-checks proof of work on it at every startup. That is a node that will not
// start, from one unsolicited header. The nonce rule is the only thing that
// keeps the two readings agreeing, which is why it cannot be optional.
BOOST_AUTO_TEST_CASE(pos_nonce_rule_matches_the_index_reading)
{
    Consensus::Params p;
    p.lastPowBlock = 999;

    // How CBlockIndex reads a nonce, and how a block reads its own coinstake.
    const auto index_says_pow = [](uint32_t nonce) { return nonce != 0; };

    for (const int height : {1000, 1001, 7559, 7560, 100000}) {
        for (const uint32_t nonce : {uint32_t{0}, uint32_t{1}, uint32_t{12345}}) {
            // Every header this rule admits at a proof-of-stake height is one
            // the index will also call proof of stake. That is the whole
            // invariant: admitted implies agreeing.
            if (CheckPosBlockNonce(height, nonce, p)) {
                BOOST_CHECK_MESSAGE(!index_says_pow(nonce),
                                    strprintf("height %d nonce %u is admitted but the index would "
                                              "call it proof of work", height, nonce));
            }
        }
    }

    // And below the boundary the disagreement is harmless, because the index
    // calling it proof of work is correct there.
    for (const int height : {1, 500, 999}) {
        BOOST_CHECK(CheckPosBlockNonce(height, 12345, p));
        BOOST_CHECK(index_says_pow(12345));
    }
}

// Same shape as the nonce rule, for the same reason: the enforcing function
// needs a masternode list and governance state, and no unit fixture reaches a
// proof-of-stake height on regtest. The decision is enumerated instead.
BOOST_AUTO_TEST_CASE(pos_coinbase_value_rule)
{
    Consensus::Params p;
    p.lastPowBlock = 999;
    const CAmount expected = 10000 * COIN;

    // Gate unset: the original rule -- any coinbase value at any height.
    p.nPosCoinbaseBoundActivationHeight = std::numeric_limits<int>::max();
    BOOST_CHECK(CheckPosCoinbaseValue(1000, expected + 100000 * COIN, expected, p));
    BOOST_CHECK(CheckPosCoinbaseValue(7560, expected + 100000 * COIN, expected, p));

    p.nPosCoinbaseBoundActivationHeight = 7560;
    // Proof-of-work heights are untouched: their coinbase is bounded elsewhere.
    BOOST_CHECK(CheckPosCoinbaseValue(500, expected + 100000 * COIN, expected, p));
    BOOST_CHECK(CheckPosCoinbaseValue(999, expected + 100000 * COIN, expected, p));
    // Proof-of-stake heights below the gate keep the original rule.
    BOOST_CHECK(CheckPosCoinbaseValue(1000, expected + 100000 * COIN, expected, p));
    BOOST_CHECK(CheckPosCoinbaseValue(7559, expected + 100000 * COIN, expected, p));
    // At and above the gate: exactly the expected payouts pass, less passes,
    // one duff more does not.
    BOOST_CHECK(CheckPosCoinbaseValue(7560, expected, expected, p));
    BOOST_CHECK(CheckPosCoinbaseValue(7560, expected - 1, expected, p));
    BOOST_CHECK(CheckPosCoinbaseValue(7560, 0, expected, p));
    BOOST_CHECK(!CheckPosCoinbaseValue(7560, expected + 1, expected, p));
    BOOST_CHECK(!CheckPosCoinbaseValue(7560, expected + 100000 * COIN, expected, p));
    BOOST_CHECK(!CheckPosCoinbaseValue(100000, expected + 1, expected, p));
    // The era with no masternode to pay expects nothing, and gets nothing.
    BOOST_CHECK(CheckPosCoinbaseValue(7560, 0, 0, p));
    BOOST_CHECK(!CheckPosCoinbaseValue(7560, 1, 0, p));

    p.nPosCoinbaseBoundActivationHeight = 0;
    BOOST_CHECK(CheckPosCoinbaseValue(999, expected + 1, expected, p));
    BOOST_CHECK(!CheckPosCoinbaseValue(1000, expected + 1, expected, p));
}

// The modifier itself is a pure function of (previous modifier, kernel), and
// the gate is a pure function of height, so both are tested as such. The
// connect-time recomputation is one call to the former under the latter; a
// fixture that reaches a proof-of-stake height on regtest does not exist.
BOOST_AUTO_TEST_CASE(pos_stake_modifier_v2)
{
    Consensus::Params p;
    p.nPosStakeModifierV2ActivationHeight = 7560;
    BOOST_CHECK(!StakeModifierFromKernel(7559, p));
    BOOST_CHECK(StakeModifierFromKernel(7560, p));
    BOOST_CHECK(StakeModifierFromKernel(100000, p));
    p.nPosStakeModifierV2ActivationHeight = std::numeric_limits<int>::max();
    BOOST_CHECK(!StakeModifierFromKernel(100000, p));

    // What the rule changes: with the kernel read, two blocks at the same
    // height that staked different outputs get different modifiers; with the
    // kernel unread -- the header-time value -- they do not, whatever they staked.
    CBlockIndex prev;
    prev.nStakeModifier = uint256S("11");
    const uint256 kernelA = uint256S("aa");
    const uint256 kernelB = uint256S("bb");
    BOOST_CHECK(ComputeStakeModifier(&prev, kernelA) != ComputeStakeModifier(&prev, kernelB));
    BOOST_CHECK_EQUAL(ComputeStakeModifier(&prev, uint256()), ComputeStakeModifier(&prev, uint256()));
    // And the degenerate form this chain carries below the gate, stated so it
    // is a documented fact rather than a surprise: null kernel, previous only.
    CDataStream ss(SER_GETHASH, 0);
    ss << uint256() << prev.nStakeModifier;
    BOOST_CHECK_EQUAL(ComputeStakeModifier(&prev, uint256()), Hash(ss));
}

BOOST_AUTO_TEST_CASE(pos_block_time_rule)
{
    Consensus::Params p;
    p.lastPowBlock = 999;
    const int64_t prev = 1700000000;

    p.nPosBlockTimeBoundActivationHeight = std::numeric_limits<int>::max();
    BOOST_CHECK(CheckPosBlockTime(1000, prev - 600, prev, p));   // backwards, original rule: allowed here
    BOOST_CHECK(CheckPosBlockTime(7560, prev, prev, p));

    p.nPosBlockTimeBoundActivationHeight = 7560;
    BOOST_CHECK(CheckPosBlockTime(500, prev - 600, prev, p));    // proof-of-work height: untouched
    BOOST_CHECK(CheckPosBlockTime(7559, prev - 600, prev, p));   // below the gate: original rule
    BOOST_CHECK(CheckPosBlockTime(7560, prev + 1, prev, p));     // strictly after: passes
    BOOST_CHECK(CheckPosBlockTime(7560, prev + 3600, prev, p));
    BOOST_CHECK(!CheckPosBlockTime(7560, prev, prev, p));        // equal: rejected
    BOOST_CHECK(!CheckPosBlockTime(7560, prev - 1, prev, p));    // backwards: rejected
    BOOST_CHECK(!CheckPosBlockTime(100000, prev - 600, prev, p));

    p.nPosBlockTimeBoundActivationHeight = 0;
    BOOST_CHECK(CheckPosBlockTime(999, prev - 1, prev, p));
    BOOST_CHECK(!CheckPosBlockTime(1000, prev - 1, prev, p));
}

BOOST_AUTO_TEST_CASE(pos_block_time_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosBlockTimeBoundActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nPosBlockTimeBoundActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nPosBlockTimeBoundActivationHeight,
                      0);
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nPosBlockTimeBoundActivationHeight,
                      7560);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_CASE(pos_stake_modifier_v2_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosStakeModifierV2ActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nPosStakeModifierV2ActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nPosStakeModifierV2ActivationHeight,
                      0);
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nPosStakeModifierV2ActivationHeight,
                      7560);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_CASE(pos_coinbase_bound_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosCoinbaseBoundActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nPosCoinbaseBoundActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nPosCoinbaseBoundActivationHeight,
                      0);
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nPosCoinbaseBoundActivationHeight,
                      7560);
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_CASE(expected_stake_time_does_not_wrap)
{
    BOOST_CHECK_EQUAL(ExpectedStakeTime(150, 0, 0), 0);
    BOOST_CHECK_EQUAL(ExpectedStakeTime(150, 1000, 0), 0);
    BOOST_CHECK_EQUAL(ExpectedStakeTime(0, 1000, 1000), 0);
    BOOST_CHECK_EQUAL(ExpectedStakeTime(150, 1000, 1000), 150);
    BOOST_CHECK_EQUAL(ExpectedStakeTime(150, 3000, 1000), 450);

    // The devnet as read on 2026-09-02: the 64-bit product is within two per
    // cent of 2^64 and the answer, 351 s, matched what the node then reported.
    const uint64_t net = 120870184417067680ULL;
    const uint64_t mine = 51525888930772651ULL;
    BOOST_CHECK_EQUAL(ExpectedStakeTime(150, net, mine), 351);

    // Past that point 64-bit arithmetic wraps and answers 47 seconds for what
    // is really 2911; stated so the reason for the 256-bit form is on record.
    const uint64_t larger_net = 1000000000000000000ULL;
    BOOST_CHECK_EQUAL(ExpectedStakeTime(150, larger_net, mine), 2911);
    BOOST_CHECK_EQUAL(static_cast<uint64_t>(150) * larger_net / mine, 47U);

    // The far end saturates instead of wrapping.
    BOOST_CHECK_EQUAL(ExpectedStakeTime(std::numeric_limits<int64_t>::max(), std::numeric_limits<uint64_t>::max(), 1),
                      std::numeric_limits<int64_t>::max());
}

BOOST_AUTO_TEST_CASE(kernel_v2_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nPosKernelV2ActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nPosKernelV2ActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nPosKernelV2ActivationHeight,
                      0);

    // Devnet params assert on a devnet name being set; pow_tests does the same dance.
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nPosKernelV2ActivationHeight,
                      4000);
    gArgs.ForceRemoveArg("devnet");
}


/**
 * The strict BLS signature-size rule is a rollout too, and unset means off.
 *
 * mainnet and testnet must stay unset until the rule is deliberately scheduled
 * -- an activation height that appeared here by accident is a consensus change
 * nobody decided to make. Devnet carries a provisional height, brought forward
 * at rollout; regtest is always on for the encoding tests.
 */
BOOST_AUTO_TEST_CASE(strict_bls_sig_size_activation_heights_are_pinned)
{
    const auto& args = *m_node.args;
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::MAIN)->GetConsensus().nStrictBLSSigSizeActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::TESTNET)->GetConsensus().nStrictBLSSigSizeActivationHeight,
                      std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::REGTEST)->GetConsensus().nStrictBLSSigSizeActivationHeight,
                      0);

    // Devnet unset since 2026-09-05: M-02 is out of the v23 release, and the
    // devnet is kept identical to what ships. Regtest keeps it at 0 so the rule
    // itself stays exercised by script_tests.
    gArgs.SoftSetBoolArg("-devnet", true);
    BOOST_CHECK_EQUAL(CreateChainParams(args, CBaseChainParams::DEVNET)->GetConsensus().nStrictBLSSigSizeActivationHeight,
                      std::numeric_limits<int>::max());
    gArgs.ForceRemoveArg("devnet");
}

BOOST_AUTO_TEST_SUITE_END()
