// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_COMMON_H
#define BITCOIN_COINJOIN_COMMON_H

#include <consensus/amount.h>
#include <primitives/transaction.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <util/ranges.h>

/** Holds a mixing input
 */
class CTxDSIn : public CTxIn
{
public:
    // memory only
    CScript prevPubKey;
    bool fHasSig{false}; // flag to indicate if signed
    int nRounds{-10};

    CTxDSIn(const CTxIn& txin, CScript script, int nRounds) :
        CTxIn(txin),
        prevPubKey(std::move(script)),
        nRounds(nRounds)
    {
    }

    CTxDSIn() = default;
};

namespace CoinJoin
{

/** The wire table: a denomination travels as the bit of its index here, so the
 *  order is fixed forever. Bits 0-4 are the amounts v23.0.0 knows; bits 5-7 the
 *  DFCN-scale amounts (100.001, 1000.01 and 10 000.1 DFCN) that every v23.1 node
 *  accepts and relays (CJ4). The protocol calls below read it (the DSTX structure
 *  check, the masternode's session checks and the session amount check,
 *  IsValidInOuts), and the wallet recognises all eight through
 *  IsKnownDenominationAmount (labels, coin types, round counts; CJ4-B). */
constexpr std::array<CAmount, 8> vecDenominationsByBit{
        (10 * COIN) + 10000,
        (1 * COIN) + 1000,
        (COIN / 10) + 100,
        (COIN / 100) + 10,
        (COIN / 1000) + 1,
        (100 * COIN) + 100000,
        (1000 * COIN) + 1000000,
        (10000 * COIN) + 10000000,
};

/** The wallet's list, in descending order: what the mixing target counts and what
 *  the wallet creates and selects for its queues. It stays the five amounts of
 *  v23.0.0; the DFCN-scale amounts are recognised from the wire table, not created.
 *  Staking excludes all eight wire-table amounts (IsKnownDenominationAmount, CJ4-B). */
constexpr std::array<CAmount, 5> vecStandardDenominations{
        (10 * COIN) + 10000,
        (1 * COIN) + 1000,
        (COIN / 10) + 100,
        (COIN / 100) + 10,
        (COIN / 1000) + 1,
};

static_assert([] {
    for (size_t i = 0; i < vecStandardDenominations.size(); ++i) {
        if (vecStandardDenominations[i] != vecDenominationsByBit[i]) return false;
    }
    return true;
}(), "the wallet's denominations must be bits 0-4 of the wire table, in the same order");

constexpr std::array<CAmount, 5> GetStandardDenominations() { return vecStandardDenominations; }
constexpr CAmount GetSmallestDenomination() { return vecStandardDenominations.back(); }

/*
    Protocol call: return the bit of an amount in vecDenominationsByBit, or 0 if it is none of them.
*/
constexpr int AmountToDenomination(CAmount nInputAmount)
{
    for (size_t i = 0; i < vecDenominationsByBit.size(); ++i) {
        if (nInputAmount == vecDenominationsByBit[i]) {
            return 1 << i;
        }
    }
    return 0;
}

/*
    Protocol call. Returns:
    - the amount of the given bit in vecDenominationsByBit
    - 0 for non-initialized sessions (nDenom = 0)
    - a value below 0 if an error occurred while converting from one to another
*/
constexpr CAmount DenominationToAmount(int nDenom)
{
    if (nDenom == 0) {
        // not initialized
        return 0;
    }

    size_t nMaxDenoms = vecDenominationsByBit.size();

    if (nDenom >= (1 << nMaxDenoms) || nDenom < 0) {
        // out of bounds
        return -1;
    }

    if ((nDenom & (nDenom - 1)) != 0) {
        // non-denom
        return -2;
    }

    CAmount nDenomAmount{-3};

    for (size_t i = 0; i < nMaxDenoms; ++i) {
        if (nDenom & (1 << i)) {
            nDenomAmount = vecDenominationsByBit[i];
            break;
        }
    }

    return nDenomAmount;
}

/** Protocol call: an amount of the wire table (any of the eight). */
constexpr bool IsKnownDenominationAmount(CAmount nInputAmount) { return AmountToDenomination(nInputAmount) > 0; }
/** Protocol call: a single bit of the wire table. */
constexpr bool IsValidDenomination(int nDenom) { return DenominationToAmount(nDenom) > 0; }

/** Wallet API: an amount of the wallet's list (the five of v23.0.0). */
constexpr bool IsDenominatedAmount(CAmount nInputAmount)
{
    for (const CAmount denom : vecStandardDenominations) {
        if (nInputAmount == denom) return true;
    }
    return false;
}

/** A bit of the wire table beyond the five of v23.0.0: bits 5-7, the DFCN-scale amounts. Read from the bit, not
 *  from the wallet's list, so it means the same whatever the wallet uses. */
constexpr bool IsDfcnScaleDenomination(int nDenom)
{
    return IsValidDenomination(nDenom) && nDenom >= (1 << 5);
}

/** Whether a masternode whose tip is at `tip_height` opens a session for a DFCN-scale bit (5-7): from the v23.1
 *  height H, counted by the height of the next block as the consensus gates count it; never while H is unset (the
 *  largest int, which the next block's height reaches at one tip). */
constexpr bool DfcnScaleSessionsOpenAt(int activation_height, int tip_height)
{
    if (activation_height == std::numeric_limits<int>::max()) return false;
    return int64_t{tip_height} + 1 >= int64_t{activation_height};
}

/** Blocks from H to U, the height from which the mixing client may mix the DFCN-scale amounts (CJ4-B): the masternodes
 *  open these bits from H, so the clients start only once the masternodes have had time to upgrade. */
static constexpr int DFCN_SCALE_MIXING_DELAY{576};

/** U, in 64 bits: H + DFCN_SCALE_MIXING_DELAY, or regtest's override (cj4use); none while H is unset, nor when the
 *  override is the largest int, which stands for never as for every activation argument. */
constexpr std::optional<int64_t> DfcnScaleMixingHeight(int activation_height, std::optional<int> override_height)
{
    if (activation_height == std::numeric_limits<int>::max()) return std::nullopt;
    if (override_height) {
        if (*override_height == std::numeric_limits<int>::max()) return std::nullopt;
        return int64_t{*override_height};
    }
    return int64_t{activation_height} + DFCN_SCALE_MIXING_DELAY;
}

/** Whether the mixing client whose tip is at `tip_height` may run a DFCN-scale attempt: from U, counted by the height
 *  of the next block as DfcnScaleSessionsOpenAt counts H. */
constexpr bool DfcnScaleMixingOpenAt(int activation_height, std::optional<int> override_height, int tip_height)
{
    const std::optional<int64_t> mixing_height{DfcnScaleMixingHeight(activation_height, override_height)};
    return mixing_height.has_value() && int64_t{tip_height} + 1 >= *mixing_height;
}

/*
Same as DenominationToAmount but returns a string representation
*/
std::string DenominationToString(int nDenom);

constexpr CAmount GetCollateralAmount() { return GetSmallestDenomination() / 10; }
constexpr CAmount GetMaxCollateralAmount() { return GetCollateralAmount() * 4; }

constexpr bool IsCollateralAmount(CAmount nInputAmount)
{
    // collateral input can be anything between 1x and "max" (including both)
    return (nInputAmount >= GetCollateralAmount() && nInputAmount <= GetMaxCollateralAmount());
}

/** A denomination or a collateral amount: the amounts CoinJoin itself spends in v23.0.0,
 *  which CoinType::ONLY_NONDENOMINATED leaves out together with the DFCN-scale amounts (CJ4). */
constexpr bool IsDenominatedOrCollateralAmount(CAmount nInputAmount)
{
    return IsDenominatedAmount(nInputAmount) || IsCollateralAmount(nInputAmount);
}

/** Any of the eight wire-table amounts or a collateral amount: what CoinType::ONLY_NONDENOMINATED
 *  leaves out. Stake combining and combineoutputs never spend an output of such an amount (CJ4-B). */
constexpr bool IsKnownDenominationOrCollateralAmount(CAmount nInputAmount)
{
    return IsKnownDenominationAmount(nInputAmount) || IsCollateralAmount(nInputAmount);
}

constexpr int CalculateAmountPriority(CAmount nInputAmount)
{
    if (auto optDenom = ranges::find_if_opt(GetStandardDenominations(), [&nInputAmount](const auto& denom) {
        return nInputAmount == denom;
    })) {
        return (float)COIN / *optDenom * 10000;
    }
    if (nInputAmount < COIN) {
        return 20000;
    }

    //nondenom return largest first
    return -1 * (nInputAmount / COIN);
}

} // namespace CoinJoin

#endif // BITCOIN_COINJOIN_COMMON_H
