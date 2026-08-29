// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_COLLATERAL_H
#define BITCOIN_MASTERNODE_COLLATERAL_H

#include <evo/deterministicmns.h>
#include <logging.h>
#include <primitives/transaction.h>

namespace Consensus { struct Params; }

// True unless `txout` is a masternode collateral registered fewer than
// minStaticCollateral blocks before nHeight. Derived from the deterministic MN
// list, so the verdict does not depend on how the node started up.
bool CheckPrematureCollateralMovement(const CDeterministicMNList& mnList, const COutPoint& txout,
                                      int nHeight, const Consensus::Params& params);

#endif // BITCOIN_MASTERNODE_COLLATERAL_H
