// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include <consensus/amount.h>
#include <script/script.h>
#include <uint256.h>
#include <llmq/params.h>

#include <limits>
#include <vector>

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CLTV,
    DEPLOYMENT_BIP147,
    DEPLOYMENT_CSV,
    DEPLOYMENT_DIP0001,
    DEPLOYMENT_DIP0003,
    DEPLOYMENT_DIP0008,
    DEPLOYMENT_DIP0020,
    DEPLOYMENT_DIP0024,
    DEPLOYMENT_BRR,
    DEPLOYMENT_V19,
    DEPLOYMENT_V20,
    DEPLOYMENT_MN_RR,
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_MN_RR; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    DEPLOYMENT_WITHDRAWALS, // Deployment of Fix for quorum selection for withdrawals
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit{28};
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime{NEVER_ACTIVE};
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout{NEVER_ACTIVE};
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};
    /** The number of past blocks (including the block under consideration) to be taken into account for locking in a fork. */
    int64_t nWindowSize{0};
    /** A starting number of blocks, in the range of 1..nWindowSize, which must signal for a fork in order to lock it in. */
    int64_t nThresholdStart{0};
    /** A minimum number of blocks, in the range of 1..nWindowSize, which must signal for a fork in order to lock it in. */
    int64_t nThresholdMin{0};
    /** A coefficient which adjusts the speed a required number of signaling blocks is decreasing from nThresholdStart to nThresholdMin at with each period. */
    int64_t nFalloffCoeff{0};
    /** This value is used for forks activated by masternodes.
      * false means it is a regular fork, no masternodes confirmation is needed.
      * true means that a signalling of masternodes is expected first to determine a height when miners signals are matter.
      */
    bool useEHF{false};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    uint256 hashDevnetGenesisBlock;
    int nSubsidyHalvingInterval;
    /** Block height at which BIP16 becomes active */
    int BIP16Height;
    int nMasternodePaymentsStartBlock;
    int nMasternodePaymentsIncreaseBlock;
    int nMasternodePaymentsIncreasePeriod; // in blocks
    int nInstantSendConfirmationsRequired; // in blocks
    int nInstantSendKeepLock; // in blocks
    int nBudgetPaymentsStartBlock;
    int nBudgetPaymentsCycleBlocks;
    int nBudgetPaymentsWindowBlocks;
    int nSuperblockStartBlock;
    uint256 nSuperblockStartHash;
    int nSuperblockCycle; // in blocks
    int nSuperblockMaturityWindow; // in blocks
    int nGovernanceMinQuorum; // Min absolute vote count to trigger an action
    int nGovernanceFilterElements;
    int nMasternodeMinimumConfirmations;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    // Deployment of BIP147 (NULLDUMMY)
    int BIP147Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which DIP0001 becomes active */
    int DIP0001Height;
    /** Block height at which DIP0002 and DIP0003 (txv3 and deterministic MN lists) becomes active */
    int DIP0003Height;
    /** Block height at which DIP0003 becomes enforced */
    int DIP0003EnforcementHeight;
    uint256 DIP0003EnforcementHash;
    /** Block height at which DIP0008 becomes active */
    int DIP0008Height;
    /** Block height at which BRR (Block Reward Reallocation) becomes active */
    int BRRHeight;
    /** Block height at which DIP0020, DIP0021 and LLMQ_100_67 quorums become active */
    int DIP0020Height;
    /** Block height at which DIP0024 (Quorum Rotation) and decreased governance proposal fee becomes active */
    int DIP0024Height;
    /** Block height at which the first DIP0024 quorum was mined */
    int DIP0024QuorumsHeight;
    /** Block height at which V19 (Basic BLS) becomes active */
    int V19Height;
    /** Block height at which V20 (Deployment of EHF, LLMQ Randomness Beacon) becomes active */
    int V20Height;
    /** Block height at which MN_RR (Deployment of Masternode Reward Location Reallocation) becomes active */
    int MN_RRHeight;
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV and DIP activations. */
    int MinBIP9WarningHeight;
    /**
     * Minimum blocks including miner confirmation of the total of nMinerConfirmationWindow blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Default BIP9Deployment::nThresholdStart value for deployments where it's not specified and for unknown deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    // Default BIP9Deployment::nWindowSize value for deployments where it's not specified and for unknown deployments.
    uint32_t nMinerConfirmationWindow;
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];
    /** Proof of work parameters */
    uint256 powLimit;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    int nPowKGWHeight;
    int nPowDGWHeight;
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    uint256 nMinimumChainWork;
    uint256 defaultAssumeValid;
    /** Canonical block used to prevent the July 2026 fork from returning after a reindex. */
    int nForkRecoveryAnchorHeight{-1};
    uint256 hashForkRecoveryAnchor;
    /**
     * Height at which every accepted chain must contain hashForkRecoveryAnchor.
     * A negative value disables this rule on non-main networks.
     */
    int nForkRecoveryActivationHeight{-1};

    /** these parameters are specific to the pacplatform extensions */
    uint256 posLimit;
    int lastPowBlock{0};
    int64_t posTargetSpacing{0};
    int64_t posTargetTimespan{0};
    uint32_t posTimestampMask{0};
    std::vector<CAmount> stakeValueRange;
    std::vector<int64_t> stakeAgeRange;
    CAmount regularMnCollateral;
    int32_t regularVoteWeight;
    int32_t regularPaymentWeight;
    CAmount evoMnCollateral;
    int32_t evoVoteWeight;
    int32_t evoPaymentWeight;
    CAmount computeMnCollateral;
    int32_t computeVoteWeight;
    int32_t computePaymentWeight;
    int minStaticCollateral;
    CScript premineAddress;
    /** Height from which the corrected proof-of-stake kernel rules apply: the
     *  weighted target stops being computed by a multiplication that truncates
     *  to 256 bits, and the upper bound of stakeAgeRange stops applying.
     *  One-way and height-only by design, so a block's rules follow from its
     *  height alone and blocks made under the original rules stay valid under
     *  them forever. The default -- an unreachable height -- means the original
     *  rules, which is what every network keeps until it is set. */
    int nPosKernelV2ActivationHeight{std::numeric_limits<int>::max()};

    /** Height from which a header at a proof-of-stake height must carry
     *  nNonce == 0. Two definitions of "is this proof of stake" coexist: the
     *  block's, which reads the coinstake, and the index's, which reads the
     *  nonce -- and the index is what LoadBlockIndexGuts consults on every
     *  restart. A coinstake block with a non-zero nonce is accepted as PoS,
     *  reloaded as PoW, and then fails CheckProofOfWork on every node at once;
     *  ChainLocked, that is permanent until a code change. Pinning the nonce at
     *  PoS heights makes the two definitions agree for every accepted block.
     *  One-way and height-only. The default -- an unreachable height -- means
     *  the original rule, which every network keeps until it is set. */
    int nPosNonceActivationHeight{std::numeric_limits<int>::max()};

    /** M-02: below this height IsBLSSig accepts any signature of BLS size or
     *  larger, letting an oversized signature skip the ECDSA encoding checks;
     *  at or above it, only the exact BLS size is treated as BLS. The default
     *  unreachable height means the original rule, which every network keeps
     *  until this is set. */
    int nStrictBLSSigSizeActivationHeight{std::numeric_limits<int>::max()};

    /** Height from which the Compute masternode type (MnType 2) may
     *  register. The type ships fully wired but dormant: below this height
     *  no ProRegTx or ProUpServTx may carry it, so it cannot enter the
     *  deterministic list, earn payouts, vote or join quorums. The default
     *  unreachable height keeps it dormant on every network until a
     *  coordinated activation sets it. */
    int nComputeNodeActivationHeight{std::numeric_limits<int>::max()};

    /** DeFCon Sentinel Layer (DSL / "Service PoSe"). nDSLActivationHeight:
     *  the height from which a service-commitment special tx may appear and
     *  the sentinel protocol runs. nDSLEnforcementHeight: at/above it the
     *  committed bitfield actually suspends rewards and bans -- the shadow
     *  window is the gap between the two, where a commitment is mined and
     *  verified but applies no penalty. Both default to an unreachable
     *  height, so DSL is dormant on every network until a coordinated
     *  activation sets them. nDSLEpochInterval: blocks per epoch, matching
     *  the attesting (Q60) quorum's DKG interval. */
    int nDSLActivationHeight{std::numeric_limits<int>::max()};
    int nDSLEnforcementHeight{std::numeric_limits<int>::max()};
    int nDSLEpochInterval{24};

    /** DSL rule constants (measured on the simulator): a masternode is
     *  reward-suspended after nDSLSuspendEpochs consecutive missed epochs and
     *  service-banned after nDSLBanEpochs; no penalty is applied in an epoch
     *  whose missed fraction reaches nDSLMassOutagePct percent (the
     *  mass-outage guard, which keeps a correlated outage from mass-banning
     *  honest nodes). */
    int nDSLSuspendEpochs{4};
    int nDSLBanEpochs{5};
    int nDSLMassOutagePct{15};
    /** How many sentinels probe each masternode per epoch, and how many must
     *  agree for a verdict (the aggregation threshold). Measured on the
     *  simulator: 7 sentinels, at-least-5 agreement resists shielding and
     *  griefing up to about 30 percent single-operator concentration. */
    int nDSLSentinelCount{7};
    int nDSLSentinelAgree{5};

    /** these parameters are only used on devnet and can be configured from the outside */
    int nMinimumDifficultyBlocks{0};
    int nHighSubsidyBlocks{0};
    int nHighSubsidyFactor{1};

    std::vector<LLMQParams> llmqs;
    LLMQType llmqTypeChainLocks;
    /** The ChainLock profile that takes over for CLSIGs whose signed height is
     *  at or above nChainLocksV2ActivationHeight (the Q60 switchover). The
     *  defaults -- no type, an unreachable height -- mean the legacy type
     *  forever; both must be set for the resolver to ever switch. */
    LLMQType llmqTypeChainLocksV2{LLMQType::LLMQ_NONE};
    int nChainLocksV2ActivationHeight{std::numeric_limits<int>::max()};
    /** Height at and above which a profile that carries a dkgBadVotesThresholdV2
     *  uses it in place of dkgBadVotesThreshold. Height-only and one-way, like
     *  the ChainLock switchover above: a DKG session must resolve the same
     *  threshold from its own quorum height alone, or its members compute
     *  different valid-member sets and agree on no commitment at all. The
     *  default -- an unreachable height -- leaves every network on the value it
     *  has today until one is set deliberately. */
    int nDkgBadVotesV2ActivationHeight{std::numeric_limits<int>::max()};
    LLMQType llmqTypeDIP0024InstantSend{LLMQType::LLMQ_NONE};
    LLMQType llmqTypePlatform{LLMQType::LLMQ_NONE};
    LLMQType llmqTypeMnhf{LLMQType::LLMQ_NONE};

    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_BIP147:
            return BIP147Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_DIP0001:
            return DIP0001Height;
        case DEPLOYMENT_DIP0003:
            return DIP0003Height;
        case DEPLOYMENT_DIP0008:
            return DIP0008Height;
        case DEPLOYMENT_DIP0020:
            return DIP0020Height;
        case DEPLOYMENT_DIP0024:
            return DIP0024Height;
        case DEPLOYMENT_BRR:
            return BRRHeight;
        case DEPLOYMENT_V19:
            return V19Height;
        case DEPLOYMENT_V20:
            return V20Height;
        case DEPLOYMENT_MN_RR:
            return MN_RRHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
