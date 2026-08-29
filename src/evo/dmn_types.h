// Copyright (c) 2023-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_DMN_TYPES_H
#define BITCOIN_EVO_DMN_TYPES_H

#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>

#include <limits>
#include <string_view>

enum class MnType : uint16_t {
    Regular = 0,
    // Retired. The value stays reserved so serialized data carrying it can
    // never be reinterpreted as a future type; no transaction may use it.
    Evo = 1,
    // The DefCompute oracle masternode. Registrable only at or above
    // Consensus::Params::nComputeNodeActivationHeight.
    Compute = 2,
    COUNT,
    Invalid = std::numeric_limits<uint16_t>::max(),
};

template<typename T> struct is_serializable_enum;
template<> struct is_serializable_enum<MnType> : std::true_type {};

namespace dmn_types {

struct mntype_struct
{
    // A masternode type carries two separate dials: voting_weight scales its
    // governance votes, payment_weight is how many consecutive payout slots it
    // occupies in the payment cycle. They share values today; nothing requires
    // that they stay equal, so every reader must name the dial it means.
    int32_t voting_weight;
    int32_t payment_weight;
    CAmount collat_amount;
    std::string_view description;
};

mntype_struct BuildMnStruct(MnType mn_type);
bool IsCollateralAmount(CAmount amount);

} // namespace dmn_types

dmn_types::mntype_struct GetMnType(MnType type);

[[nodiscard]] constexpr bool IsValidMnType(MnType type) { return type < MnType::COUNT; }

/** The one resolver for whether the Compute type may exist at a height.
 *  Every enforcement point funnels through this, so activation is a single
 *  one-way, height-only switch. */
[[nodiscard]] constexpr bool IsComputeTypeActive(int nHeight, const Consensus::Params& params)
{
    return nHeight >= params.nComputeNodeActivationHeight;
}

#endif // BITCOIN_EVO_DMN_TYPES_H
