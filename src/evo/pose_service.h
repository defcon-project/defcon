// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_H
#define BITCOIN_EVO_POSE_SERVICE_H

#include <bls/bls.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <algorithm>
#include <vector>

class CBlockIndex;
class ChainstateManager;
class TxValidationState;
class UniValue;
namespace llmq {
class CQuorumManager;
} // namespace llmq

/**
 * The DeFCon Sentinel Layer (DSL / "Service PoSe") commitment.
 *
 * One per epoch: a bitfield of which masternodes were observed MISSED that
 * epoch, threshold-signed by the attesting quorum. The chain verifies ONLY the
 * quorum signature -- the ChainLock trust model. The bitfield's truth rests on
 * the quorum, not on the chain re-deriving liveness, exactly as a CLSIG's truth
 * rests on the quorum. Bit i refers to the masternode at canonical index i in
 * the deterministic list at epochBlockHash (sorted by proTxHash); the list of
 * proTxHashes is therefore not carried here.
 */
class CPoSeServiceCommitment
{
public:
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t nVersion{CURRENT_VERSION};
    uint32_t nEpoch{0};
    uint256 epochBlockHash;
    Consensus::LLMQType llmqType{Consensus::LLMQType::LLMQ_NONE};
    uint256 quorumHash;
    std::vector<bool> missed;
    CBLSSignature quorumSig;

    SERIALIZE_METHODS(CPoSeServiceCommitment, obj)
    {
        READWRITE(obj.nVersion, obj.nEpoch, obj.epochBlockHash, obj.llmqType,
                  obj.quorumHash, DYNBITSET(obj.missed));
        READWRITE(CBLSSignatureVersionWrapper(const_cast<CBLSSignature&>(obj.quorumSig), /*fLegacy=*/false));
    }

    [[nodiscard]] int CountMissed() const { return int(std::count(missed.begin(), missed.end(), true)); }

    /** The signing-session id, binding the recovered threshold sig to this epoch. */
    [[nodiscard]] uint256 GetRequestId() const;

    /**
     * Verify the quorum threshold signature over `msgHash` (the transaction with
     * the signature field zeroed, computed by the caller). Nothing else is
     * checked here -- the bitfield is the quorum's assertion, not a chain fact.
     */
    [[nodiscard]] bool Verify(const llmq::CQuorumManager& qman, const uint256& msgHash,
                              TxValidationState& state) const;

    [[nodiscard]] UniValue ToJson() const;
};

class CPoSeServiceCommitmentTxPayload
{
public:
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_POSE_SERVICE_COMMITMENT;
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t nVersion{CURRENT_VERSION};
    CPoSeServiceCommitment commitment;

    SERIALIZE_METHODS(CPoSeServiceCommitmentTxPayload, obj)
    {
        READWRITE(obj.nVersion, obj.commitment);
    }

    [[nodiscard]] UniValue ToJson() const;
};

/**
 * Consensus check for a service-commitment special transaction: it is rejected
 * below the activation height (the type ships dormant), it may appear only at an
 * epoch-boundary height with a matching epoch and epoch-base hash, its attesting
 * quorum type must be the ChainLock quorum resolved at that height, and its
 * quorum threshold signature must verify. The bitfield is NOT applied to
 * masternode state here -- that is a later step.
 */
[[nodiscard]] bool CheckPoSeServiceCommitmentTx(const ChainstateManager& chainman,
                                                const llmq::CQuorumManager& qman,
                                                const CTransaction& tx,
                                                const CBlockIndex* pindexPrev,
                                                TxValidationState& state);

#endif // BITCOIN_EVO_POSE_SERVICE_H
