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
    Evo = 1,
    COUNT,
    Invalid = std::numeric_limits<uint16_t>::max(),
};

template<typename T> struct is_serializable_enum;
template<> struct is_serializable_enum<MnType> : std::true_type {};

namespace dmn_types {

struct mntype_struct
{
    int32_t voting_weight;
    CAmount collat_amount;
    std::string_view description;
};

mntype_struct BuildMnStruct(MnType mn_type);
bool IsCollateralAmount(CAmount amount);

} // namespace dmn_types

dmn_types::mntype_struct GetMnType(MnType type);

[[nodiscard]] constexpr bool IsValidMnType(MnType type) { return type < MnType::COUNT; }

#endif // BITCOIN_EVO_DMN_TYPES_H
