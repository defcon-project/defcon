// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/collateral.h>

Mutex cs_mncache;
std::map<COutPoint, int> mapCollaterals GUARDED_BY(cs_mncache);

void MaintainCollateralCache(const CDeterministicMNList& mnList)
{
    LOCK(cs_mncache);

    mapCollaterals.clear();
    mnList.ForEachMN(false, [&](auto& dmn) {
        COutPoint mnOutpoint(dmn.collateralOutpoint);
        int mnRegHeight(dmn.pdmnState->nRegisteredHeight);
        mapCollaterals.insert({mnOutpoint, mnRegHeight});
    });
}

bool CheckPrematureCollateralMovement(const COutPoint& txout, int nHeight, const Consensus::Params& params)
{
    LOCK(cs_mncache);

    if (!mapCollaterals.size())
        return true;

    auto it = mapCollaterals.find(txout);
    if (it != mapCollaterals.end()) {
        int elapsedSinceReg = nHeight - it->second;
        if (elapsedSinceReg < params.minStaticCollateral) {
            int blocksTillMovement = params.minStaticCollateral - elapsedSinceReg;
            LogPrint(BCLog::MNPAYMENTS, "%s -- masternode collateral (%s) attempt to spend (%d/%d blocks)\n",
                                        __func__, txout.ToString(), elapsedSinceReg, params.minStaticCollateral);
            return false;
        }
    }

    return true;
}
