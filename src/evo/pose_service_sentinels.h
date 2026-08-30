// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_SENTINELS_H
#define BITCOIN_EVO_POSE_SERVICE_SENTINELS_H

#include <bls/bls.h>
#include <consensus/params.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

class CDeterministicMNList;
class CPoSeServiceCommitment;

namespace dsl {

enum class ServiceStatus : uint8_t {
    UNKNOWN = 0, // too few reachable sentinels to reach a verdict
    ONLINE = 1,
    MISSED = 2,
};

/**
 * The `count` masternodes deterministically assigned to probe `target` this
 * epoch: the lowest-scoring confirmed masternodes under a per-target, per-epoch
 * modifier, excluding the target itself. Confirmed-only, so a fresh
 * registration cannot grind its way into a target's sentinel set, and the
 * modifier rotates every epoch. This is off-chain: nodes use it to decide whom
 * to probe and quorum members to build their local view; it is never checked in
 * block validation. Selection reuses the production quorum-scoring function, so
 * a masternode that is PoSe-banned (any origin) is never chosen as a sentinel.
 */
std::vector<uint256> CalcSentinelsForMN(const CDeterministicMNList& list,
                                        const uint256& targetProTxHash,
                                        const uint256& epochBlockHash,
                                        size_t count);

/**
 * The targets `sentinelProTxHash` must probe this epoch: every masternode
 * whose assigned sentinel set (CalcSentinelsForMN) contains it. This is the
 * inverse of the assignment -- a node probes exactly those masternodes that
 * the epoch selected it to watch -- and is what a masternode iterates each
 * epoch to decide whom to challenge. Off-chain, like the assignment it mirrors.
 */
std::vector<uint256> GetProbeTargetsForSentinel(const CDeterministicMNList& list,
                                                const uint256& sentinelProTxHash,
                                                const uint256& epochBlockHash,
                                                size_t count);

/**
 * The challenge nonce a sentinel sends its target: bound to the epoch base and
 * the target so a captured response cannot be replayed into another epoch or
 * against another node.
 */
uint256 ServiceChallengeNonce(const uint256& epochBlockHash, const uint256& targetProTxHash);

/**
 * A target proves it is alive this epoch by signing its own challenge nonce
 * with its operator BLS key -- the same key it signs quorum shares with. The
 * nonce binds the proof to the epoch base and the target, so a captured
 * response cannot be replayed into another epoch or on another node's behalf.
 * A sentinel that receives a verifying response records the target ONLINE;
 * silence past the epoch cutoff is what it reports as MISSED.
 */
CBLSSignature SignChallengeResponse(const CBLSSecretKey& targetOperatorKey,
                                    const uint256& epochBlockHash, const uint256& targetProTxHash);
[[nodiscard]] bool VerifyChallengeResponse(const CBLSSignature& sig, const CBLSPublicKey& targetOperatorKey,
                                           const uint256& epochBlockHash, const uint256& targetProTxHash);

/**
 * A sentinel's signed observation of one target for one epoch. Gossiped between
 * masternodes, never put on chain: the quorum's threshold signature over the
 * aggregated bitfield is the only on-chain artefact. Signed with the sentinel's
 * operator BLS key -- the same key it signs quorum shares with.
 */
class CPoSeServiceReport
{
public:
    uint32_t nEpoch{0};
    uint256 targetProTxHash;
    uint256 sentinelProTxHash;
    uint8_t status{static_cast<uint8_t>(ServiceStatus::UNKNOWN)};
    CBLSSignature sig;

    SERIALIZE_METHODS(CPoSeServiceReport, obj)
    {
        READWRITE(obj.nEpoch, obj.targetProTxHash, obj.sentinelProTxHash, obj.status);
        READWRITE(CBLSSignatureVersionWrapper(const_cast<CBLSSignature&>(obj.sig), /*fLegacy=*/false));
    }

    [[nodiscard]] uint256 GetSignHash() const;
    void Sign(const CBLSSecretKey& operatorKey);
    [[nodiscard]] bool VerifySig(const CBLSPublicKey& operatorPubKey) const;
};

/**
 * Wire message: a sentinel asks a target to prove it is alive for an epoch.
 * The nonce is deterministic from (epoch base, target), so the challenge need
 * only name the epoch -- the target derives the rest and signs it.
 */
class CPoSeServiceChallenge
{
public:
    uint32_t nEpoch{0};

    SERIALIZE_METHODS(CPoSeServiceChallenge, obj) { READWRITE(obj.nEpoch); }
};

/**
 * Wire message: a target's signed liveness proof for an epoch. The responder's
 * proTxHash is not on the wire -- it comes from the authenticated masternode
 * connection the response arrived on, so a peer cannot answer for another node.
 */
class CPoSeServiceResponse
{
public:
    uint32_t nEpoch{0};
    CBLSSignature sig;

    SERIALIZE_METHODS(CPoSeServiceResponse, obj)
    {
        READWRITE(obj.nEpoch);
        READWRITE(CBLSSignatureVersionWrapper(const_cast<CBLSSignature&>(obj.sig), /*fLegacy=*/false));
    }
};

/**
 * Aggregate a set of signed sentinel reports into an unsigned service
 * commitment for the epoch. For each masternode in canonical (proTxHash)
 * order the MISSED bit is set only when at least nDSLSentinelAgree of its
 * assigned sentinels returned a valid, signed MISSED observation; a target
 * with too few such reports, or a majority reporting online, is left unset
 * (no verdict is not a punishment). A report counts only once per sentinel,
 * only from an assigned sentinel, and only with a signature that verifies
 * against that sentinel's operator key. Deterministic in its inputs -- the
 * quorum members must feed it the same report set to sign the same bitfield,
 * which the shadow phase measures. The returned commitment carries no
 * signature; the quorum signs it.
 */
CPoSeServiceCommitment BuildServiceCommitment(uint32_t nEpoch, const uint256& epochBlockHash,
                                              Consensus::LLMQType llmqType, const uint256& quorumHash,
                                              const std::vector<CPoSeServiceReport>& reports,
                                              const CDeterministicMNList& epochBaseList,
                                              const Consensus::Params& params);

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_SENTINELS_H
