// Copyright (c) 2025 The Pacplatform Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KERNEL_H
#define KERNEL_H

#include <chainparams.h>
#include <consensus/params.h>
#include <rpc/blockchain.h>
#include <validation.h>

/**
 * Check whether the coinstake timestamp meets protocol
 */
bool CheckCoinStakeTimestamp(int nTimeBlock, const Consensus::Params& params);

/**
 * Calculate PoS kernel weight for an interval of prior blocks
 */
double GetPoSKernelPS(CBlockIndex *pindex, const Consensus::Params& params);

/**
 * Compute the hash modifier for proof-of-stake
 */
uint256 ComputeStakeModifier(const CBlockIndex *pindexPrev, const uint256 &kernel);

/**
 * Whether the corrected kernel rules apply to a block at nHeight. One-way and
 * height-only by design: the staker and every validator have to reach the same
 * answer from the height alone, never from local configuration.
 */
[[nodiscard]] inline bool IsPosKernelV2(const Consensus::Params& params, int nHeight)
{
    return nHeight >= params.nPosKernelV2ActivationHeight;
}

/**
 * Check whether stake kernel meets hash target
 * Sets hashProofOfStake on success return
 *
 * Takes the consensus parameters rather than reading the global chainparams so
 * that the rules in force can be stated by the caller -- which is what lets a
 * test drive both sides of the activation height.
 */
bool CheckStakeKernelHash(const CBlockIndex *pindexPrev, const Consensus::Params &params,
    uint32_t nBits, uint32_t nBlockFromTime,
    CAmount prevOutAmount, const COutPoint &prevout, uint32_t nTimeTx,
    uint256 &hashProofOfStake, uint256 &targetProofOfStake,
    bool fPrintProofOfStake=false);

/**
 * Get kernel hash and value for blockindex and coinstake tx
 */
bool GetKernelInfo(const CBlockIndex *blockindex, const CTransaction &tx, uint256 &hash, CAmount &value, CScript &script, uint256 &blockhash);

/**
 * Check kernel hash target and coinstake signature
 * Sets hashProofOfStake on success return
 */
bool CheckProofOfStake(CChainState& chain_state, BlockValidationState& state, const CBlockIndex *pindexPrev, const CTransaction &tx, int64_t nTime, unsigned int nBits, uint256 &hashProofOfStake, uint256 &targetProofOfStake) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Wrapper around CheckStakeKernelHash()
 * Also checks existence of kernel input and min age
 * Convenient for searching a kernel
 */
bool CheckKernel(CChainState& chain_state, const CBlockIndex *pindexPrev, unsigned int nBits, int64_t nTime, const COutPoint &prevout, int64_t* pBlockTime = nullptr);

#endif // KERNEL_H
