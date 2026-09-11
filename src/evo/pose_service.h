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
#include <univalue.h>
#include <util/underlying.h>

#include <algorithm>
#include <vector>

class CBlockIndex;
class ChainstateManager;
class TxValidationState;
namespace llmq {
class CQuorumManager;
} // namespace llmq

/**
 * The DeFCon Sentinel Layer (DSL / "Service PoSe") commitment.
 *
 * One per epoch: a bitfield of which masternodes were observed MISSED that
 * epoch -- and, from format version 2, which were observed at all --
 * threshold-signed by the attesting quorum. The chain verifies ONLY the
 * quorum signature -- the ChainLock trust model. The bitfields' truth rests on
 * the quorum, not on the chain re-deriving liveness, exactly as a CLSIG's truth
 * rests on the quorum. Bit i refers to the masternode at canonical index i in
 * the deterministic list at epochBlockHash (sorted by proTxHash); the list of
 * proTxHashes is therefore not carried here.
 *
 * Why two versions. Version 1 has one bitfield, so a masternode the epoch's
 * sentinels could not judge -- too few reports either way -- is
 * indistinguishable from one they saw online, and applying the commitment
 * heals it: counter reset, suspension lifted, a service ban revived. An epoch
 * with thin sentinel coverage therefore cleared everyone. Version 2 says which
 * bits carry a verdict, and a bit without one changes nothing.
 */
class CPoSeServiceCommitment
{
public:
    /** The `missed` bitfield alone. Kept verifiable forever for the history
     *  that carries it; never produced at a height that requires version 2. */
    static constexpr uint16_t LEGACY_VERSION{1};
    /** `missed` plus `observed`. */
    static constexpr uint16_t OBSERVED_VERSION{2};
    static constexpr uint16_t CURRENT_VERSION{OBSERVED_VERSION};

    uint16_t nVersion{CURRENT_VERSION};
    uint32_t nEpoch{0};
    uint256 epochBlockHash;
    Consensus::LLMQType llmqType{Consensus::LLMQType::LLMQ_NONE};
    uint256 quorumHash;
    std::vector<bool> missed;
    /** Version 2 only: same length and index as `missed`, and a superset of
     *  it -- a masternode cannot be missed without having been observed.
     *  Absent from the bytes of a version-1 commitment, which observed
     *  everyone by construction. */
    std::vector<bool> observed;
    CBLSSignature quorumSig;

    SERIALIZE_METHODS(CPoSeServiceCommitment, obj)
    {
        READWRITE(obj.nVersion, obj.nEpoch, obj.epochBlockHash, obj.llmqType,
                  obj.quorumHash, DYNBITSET(obj.missed));
        // Version-1 bytes are exactly what they were: the field is absent, not
        // empty, so every commitment already on a chain deserializes and
        // hashes as before. The threshold signature covers whatever is
        // serialized, so on version 2 the observed bits are signed with the
        // missed ones and cannot be flipped under a valid signature.
        if (obj.nVersion >= OBSERVED_VERSION) {
            READWRITE(DYNBITSET(obj.observed));
        }
        READWRITE(CBLSSignatureVersionWrapper(const_cast<CBLSSignature&>(obj.quorumSig), /*fLegacy=*/false));
    }

    [[nodiscard]] int CountMissed() const { return int(std::count(missed.begin(), missed.end(), true)); }
    /** Whether the epoch reached a verdict on canonical index i. A version-1
     *  commitment answers yes for every index: that is what its format means,
     *  and applying it must keep meaning that for the history it sits in. */
    [[nodiscard]] bool IsObserved(size_t i) const
    {
        return nVersion < OBSERVED_VERSION || (i < observed.size() && observed[i]);
    }
    [[nodiscard]] int CountUnobserved() const
    {
        if (nVersion < OBSERVED_VERSION) return 0;
        return int(std::count(observed.begin(), observed.end(), false));
    }

    /** The signing-session id, binding the recovered threshold sig to this epoch. */
    [[nodiscard]] uint256 GetRequestId() const;

    /**
     * Verify the quorum threshold signature over `msgHash` (the transaction with
     * the signature field zeroed, computed by the caller). Nothing else is
     * checked here -- the bitfield is the quorum's assertion, not a chain fact.
     */
    [[nodiscard]] bool Verify(const llmq::CQuorumManager& qman, const uint256& msgHash,
                              TxValidationState& state) const;

    // Defined inline, like the other special-transaction payloads (mnhftx.h,
    // commitment.h): core_write.cpp lives in libbitcoin_common, so an
    // out-of-line body in the server library leaves defcon-tx unable to link.
    [[nodiscard]] UniValue ToJson() const
    {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("version", nVersion);
        obj.pushKV("epoch", (int64_t)nEpoch);
        obj.pushKV("epochBlockHash", epochBlockHash.ToString());
        obj.pushKV("llmqType", ToUnderlying(llmqType));
        obj.pushKV("quorumHash", quorumHash.ToString());
        obj.pushKV("missedCount", CountMissed());
        obj.pushKV("size", (int64_t)missed.size());
        // The set bits by canonical index, so an observer can resolve WHO was
        // missed against the deterministic list at epochBlockHash without
        // reimplementing the bitfield's serialization.
        UniValue missedIndices(UniValue::VARR);
        for (size_t i = 0; i < missed.size(); ++i) {
            if (missed[i]) missedIndices.push_back((int64_t)i);
        }
        obj.pushKV("missedIndices", missedIndices);
        // Who was judged at all. Reported for version 1 too, in the same
        // shape -- everyone observed, nobody unobserved -- so a reader needs
        // no version branch.
        obj.pushKV("observedCount", (int64_t)missed.size() - CountUnobserved());
        UniValue unobservedIndices(UniValue::VARR);
        if (nVersion >= OBSERVED_VERSION) {
            for (size_t i = 0; i < observed.size(); ++i) {
                if (!observed[i]) unobservedIndices.push_back((int64_t)i);
            }
        }
        obj.pushKV("unobservedIndices", unobservedIndices);
        return obj;
    }
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

    [[nodiscard]] UniValue ToJson() const
    {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("version", nVersion);
        obj.pushKV("commitment", commitment.ToJson());
        return obj;
    }
};

/**
 * The value that picks an epoch's attesting quorum out of the active set.
 *
 * Shared deliberately: the signer selects the quorum with it and the block rule
 * re-derives the same selection to check the commitment came from that quorum
 * and no other. Two copies of this expression would be free to drift, and the
 * one thing a drift would silently produce is a commitment the network signs
 * and then refuses.
 *
 * It names only the epoch. The epoch base joins the *request id* instead
 * (GetRequestId), so a reorg that gives the epoch a new base opens a fresh
 * signing session against the same quorum rather than a different quorum.
 */
[[nodiscard]] uint256 ServiceCommitmentQuorumSelectionHash(uint32_t nEpoch);

/**
 * The commitment format a block at `height` must carry: OBSERVED_VERSION at
 * or above nDSLCommitmentV2Height, LEGACY_VERSION below it. The signer builds
 * this version and the block rule requires exactly it. A chain never has two
 * formats valid at one height -- that would let a signer pick the one that
 * heals -- so the flip is a fleet upgrade like every other gated rule.
 */
[[nodiscard]] uint16_t RequiredServiceCommitmentVersion(const Consensus::Params& consensus, int height);

/**
 * The version-2 bitfield invariants, pure and checkable without a chain:
 * `observed` is exactly as long as `missed`, and every missed bit is an
 * observed bit. A version-1 commitment has nothing to check here.
 */
[[nodiscard]] bool CheckServiceCommitmentBitfields(const CPoSeServiceCommitment& c, TxValidationState& state);

/**
 * Consensus check for a service-commitment special transaction: it is rejected
 * below the activation height (the type ships dormant), it may appear only at an
 * epoch-boundary height, it closes the observation epoch that ended at that
 * boundary -- nEpoch is that epoch's number and epochBlockHash the hash of its
 * first block, the one the sentinel selection and challenge nonces were keyed
 * on -- its attesting quorum must be the one the epoch selects from the
 * ChainLock quorum type resolved at that height, and that quorum's threshold
 * signature must verify. It must carry exactly the format version the height
 * requires, with the version-2 bitfield invariants holding. The bitfield is
 * NOT applied to masternode state here -- that is a later step.
 */
[[nodiscard]] bool CheckPoSeServiceCommitmentTx(const ChainstateManager& chainman,
                                                const llmq::CQuorumManager& qman,
                                                const CTransaction& tx,
                                                const CBlockIndex* pindexPrev,
                                                TxValidationState& state);

#endif // BITCOIN_EVO_POSE_SERVICE_H
