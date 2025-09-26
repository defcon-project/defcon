// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/collateral.h>

Mutex cs_mncache;
std::map<COutPoint, int> mapCollaterals GUARDED_BY(cs_mncache);

void MaintainCollateralCache(COutPoint& outpoint, int nHeight)
{
    LOCK(cs_mncache);

    COutPoint mnOutpoint(outpoint);
    int mnRegHeight(nHeight);
    if (mapCollaterals.find(mnOutpoint) == mapCollaterals.end()) {
        mapCollaterals.insert({mnOutpoint, mnRegHeight});
    }
}

void MaintainCollateralCache(const CDeterministicMNList& mnList)
{
    LOCK(cs_mncache);

    mnList.ForEachMN(false, [&](auto& dmn) {
        COutPoint mnOutpoint(dmn.collateralOutpoint);
        int mnRegHeight(dmn.pdmnState->nRegisteredHeight);
        if (mapCollaterals.find(mnOutpoint) == mapCollaterals.end()) {
            mapCollaterals.insert({mnOutpoint, mnRegHeight});
        }
    });
}

void PrescanOnClientInitialise(const CBlockIndex* pscan, const Consensus::Params& params)
{
    CBlock scanBlock;
    while (!fReindex) {
        if (pscan->nHeight <= params.DIP0003Height) break;
        if (ReadBlockFromDisk(scanBlock, pscan, params)) {
            for (int i=0; i<scanBlock.vtx.size(); i++) {
                CTransactionRef tx(scanBlock.vtx[i]);
                if (tx->IsSpecialTxVersion() && tx->nType == TRANSACTION_PROVIDER_REGISTER) {
                    const auto opt_proTx = GetTxPayload<CProRegTx>(*tx);
                    COutPoint collateralOutpoint(opt_proTx->collateralOutpoint);
                    MaintainCollateralCache(collateralOutpoint, pscan->nHeight);
                }
            }
        }
        pscan = pscan->pprev;
    }
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
