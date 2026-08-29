// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/collateral.h>

#include <consensus/params.h>

bool CheckPrematureCollateralMovement(const CDeterministicMNList& mnList, const COutPoint& txout,
                                      int nHeight, const Consensus::Params& params)
{
    // The registration height comes from the deterministic list the node keeps
    // reorg-correct, so every node reaches the same verdict for the same block.
    // This replaced a process-local std::map filled at startup by a disk walk
    // that did not run during reindex, leaving the rule silently off -- two
    // nodes with different startup history could judge the same block
    // differently, which is a fork, not a missing check.
    const auto dmn = mnList.GetMNByCollateral(txout);
    if (dmn == nullptr) {
        return true;
    }
    const int elapsedSinceReg = nHeight - dmn->pdmnState->nRegisteredHeight;
    if (elapsedSinceReg < params.minStaticCollateral) {
        LogPrint(BCLog::MNPAYMENTS, "%s -- masternode collateral (%s) attempt to spend (%d/%d blocks)\n",
                 __func__, txout.ToString(), elapsedSinceReg, params.minStaticCollateral);
        return false;
    }
    return true;
}
