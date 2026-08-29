// Copyright (c) 2023-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/dmn_types.h>

namespace dmn_types {

mntype_struct BuildMnStruct(MnType mn_type)
{
    const Consensus::Params& params = Params().GetConsensus();

    mntype_struct this_mn;
    switch (mn_type) {
        case MnType::Regular:
            this_mn.voting_weight = params.regularVoteWeight;
            this_mn.payment_weight = params.regularPaymentWeight;
            this_mn.collat_amount = params.regularMnCollateral;
            this_mn.description = "Regular";
            break;
        case MnType::Evo:
            this_mn.voting_weight = params.evoVoteWeight;
            this_mn.payment_weight = params.evoPaymentWeight;
            this_mn.collat_amount = params.evoMnCollateral;
            this_mn.description = "Evo";
            break;
        case MnType::Invalid:
            this_mn.voting_weight = 0;
            this_mn.payment_weight = 0;
            this_mn.collat_amount = MAX_MONEY;
            this_mn.description = "Invalid";
            break;
    }
    return this_mn;
}

bool IsCollateralAmount(CAmount amount)
{
    return amount == dmn_types::BuildMnStruct(MnType::Regular).collat_amount ||
        amount == dmn_types::BuildMnStruct(MnType::Evo).collat_amount;
}

} // namespace dmn_types

dmn_types::mntype_struct GetMnType(MnType type)
{
    switch (type) {
        case MnType::Regular: return dmn_types::BuildMnStruct(MnType::Regular);
        case MnType::Evo: return dmn_types::BuildMnStruct(MnType::Evo);
        default: return dmn_types::BuildMnStruct(MnType::Invalid);
    }
}
