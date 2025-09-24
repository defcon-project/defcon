// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_COLLATERAL_H
#define BITCOIN_MASTERNODE_COLLATERAL_H

#include <chainparams.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <logging.h>
#include <masternode/node.h>
#include <masternode/payments.h>
#include <node/blockstorage.h>

void MaintainCollateralCache(COutPoint& outpoint, int nHeight);
void MaintainCollateralCache(const CDeterministicMNList& mnList);
void PrescanOnClientInitialise(const CBlockIndex* pscan, const Consensus::Params& params);
bool CheckPrematureCollateralMovement(const COutPoint& txout, int nHeight, const Consensus::Params& params);

#endif // BITCOIN_MASTERNODE_COLLATERAL_H

