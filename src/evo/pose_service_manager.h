// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_MANAGER_H
#define BITCOIN_EVO_POSE_SERVICE_MANAGER_H

#include <evo/pose_service_sentinels.h>
#include <evo/pose_service_store.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

class CBLSSecretKey;
class CDeterministicMNList;
namespace Consensus {
struct Params;
}

namespace dsl {

/**
 * The per-epoch state a masternode keeps to run the service probe: which of its
 * assigned targets have answered this epoch, and the report store it fills. This
 * is the off-chain engine the network layer drives -- it decides what to
 * challenge, ingests the answers and peers' reports, and emits this node's own
 * signed reports at the epoch cutoff -- but holds no sockets itself, so it is
 * exercised in isolation.
 *
 * One epoch is live at a time. BeginEpoch rolls the window forward and clears
 * the previous epoch's answers; ProcessResponse ingests a verified liveness
 * proof; ProcessReport ingests a peer's report into the relay pool; EmitReports
 * turns "probed and answered" into ONLINE and "probed and silent" into MISSED.
 * The store it owns is the relay pool the aggregator later reads.
 */
class CPoSeServiceManager
{
public:
    /** Signs a hash with this node's operator key without exposing the key --
     *  the live node passes CActiveMasternodeManager::Sign, tests a lambda. */
    using SignerFn = std::function<CBLSSignature(const uint256&)>;

    explicit CPoSeServiceManager(uint32_t keepEpochs = 8) : m_store(keepEpochs), m_keepEpochs(keepEpochs) {}

    CServiceReportStore& Store() { return m_store; }
    const CServiceReportStore& Store() const { return m_store; }

    /** What BeginEpoch did, so the caller can re-run its per-epoch actions. */
    enum class EpochChange {
        None,     //!< already on this exact (epoch, base hash)
        Entered,  //!< a higher epoch number: ordinary forward progress
        Rebased,  //!< same epoch, but a reorg swapped its base block
        Rewound,  //!< a reorg moved the tip back across an epoch boundary
    };

    /**
     * Enter an epoch on a given base block: advance the store window and forget
     * the previous epoch's responses. Idempotent for the same (epoch, base
     * hash), so a per-block tick can call it unconditionally.
     *
     * A reorg is detected two ways, and both discard the stale state -- it was
     * about a chain that no longer exists, and left in place it would feed the
     * new base's verdict and block fresh announcements as duplicates:
     *  - Rebased: the epoch number is unchanged but its base block swapped; the
     *    responses and reports for that one epoch are dropped.
     *  - Rewound: the tip moved back across a boundary to a lower epoch; every
     *    epoch from the new one up to the old tip was observed on the abandoned
     *    chain, so all of their responses and reports are dropped.
     * In both cases the caller re-runs the once-per-epoch announce/emit/sign,
     * and treats the reorged epoch as a warm-up it cannot fairly judge.
     */
    EpochChange BeginEpoch(uint32_t nEpoch, const uint256& epochBlockHash);

    /** The epoch and epoch-base hash currently being probed. */
    uint32_t CurrentEpoch() const;
    uint256 CurrentEpochHash() const;

    /**
     * The targets this node still owes a challenge this epoch: its probe set
     * (GetProbeTargetsForSentinel) minus those already recorded as responding.
     */
    std::vector<uint256> PendingChallenges(const CDeterministicMNList& list,
                                           const uint256& myProTxHash,
                                           const Consensus::Params& params) const;

    /** Record a verified liveness response from `target` for the current epoch. */
    void RecordResponse(const uint256& target);

    /**
     * This node's own liveness announcement for the current epoch, for the
     * caller to flood to its peers: the epoch, this node's proTxHash, and the
     * challenge nonce signed with its operator key.
     */
    CPoSeServiceResponse AnnounceLiveness(const uint256& myProTxHash,
                                          const CBLSSecretKey& myOperatorKey) const;
    CPoSeServiceResponse AnnounceLiveness(const uint256& myProTxHash, const SignerFn& signer) const;

    /**
     * Ingest a liveness announcement received from the flood. The caller derives
     * `epochBaseHash` for the announcement's own epoch from its active chain --
     * verification binds to the chain, not to this manager's tick, so an
     * announcement that outruns the local epoch tick is still accepted rather
     * than lost (the flood forwards each copy only once, so a rejection here is
     * permanent). Accepts only an epoch inside the retained window, from a
     * masternode on the list, with a signature that verifies against that
     * masternode's operator key -- claiming another node's identity fails on its
     * key. On first sight the node is recorded as responding for that epoch and
     * true is returned, telling the caller to relay it onward; a duplicate or
     * invalid announcement returns false.
     */
    bool ProcessResponse(const CPoSeServiceResponse& resp, const CDeterministicMNList& list,
                         const uint256& epochBaseHash);

    /**
     * Ingest a signed report received on the wire into the relay pool, with the
     * epoch-base hash for the report's epoch derived by the caller from its
     * active chain -- the same tick-independence as ProcessResponse. Returns
     * whether it was newly accepted -- the caller then relays it onward.
     */
    bool ProcessReport(const CPoSeServiceReport& report, const CDeterministicMNList& list,
                       const uint256& epochBaseHash, const Consensus::Params& params);

    /**
     * This node's signed reports for the current epoch: for every target it was
     * assigned to probe, ONLINE if that target responded and MISSED otherwise,
     * each signed with this node's operator key. The caller stores and relays
     * them. A node that was assigned no targets emits nothing.
     */
    std::vector<CPoSeServiceReport> EmitReports(const CDeterministicMNList& list,
                                                const uint256& myProTxHash,
                                                const CBLSSecretKey& myOperatorKey,
                                                const Consensus::Params& params) const;
    std::vector<CPoSeServiceReport> EmitReports(const CDeterministicMNList& list,
                                                const uint256& myProTxHash, const SignerFn& signer,
                                                const Consensus::Params& params) const;

    /** How many masternodes have announced liveness this epoch. */
    size_t RespondedCount() const;
    /** Whether this masternode's announcement was already seen this epoch. */
    bool HasResponded(const uint256& proTxHash) const;

    /**
     * Keep the commitment this node asked the quorum to sign, under the hash it
     * was signed as. A member's pool keeps growing after it signs, so when the
     * recovered signature arrives it is this commitment -- not a rebuild from
     * the pool -- that matches it, and that the member relays.
     */
    void RememberSigningCandidate(const CPoSeServiceCommitment& commitment, const uint256& msgHash);
    std::optional<CPoSeServiceCommitment> SigningCandidate(uint32_t nEpoch, const uint256& msgHash) const;

    /**
     * Keep a commitment whose quorum signature the caller has verified, for a
     * block producer whose own pool no longer reproduces what was signed.
     * Returns true only the first time a given (epoch, hash) is kept -- the
     * caller relays it then and not again, which ends the flood -- and false
     * for a duplicate or once kSignedCommitmentsPerEpoch are held for that
     * epoch. Honest operation produces one per epoch: every member is locked
     * to a single message hash, so two signed variants need members that
     * signed twice -- at least 2 * threshold - size of them, 22 on a 60/41
     * quorum. A producer holding its own recovered signature picks by that
     * signature's hash; one without it takes the first kept for its base.
     */
    bool StoreSignedCommitment(const CPoSeServiceCommitment& commitment, const uint256& msgHash);
    bool HasSignedCommitment(uint32_t nEpoch, const uint256& msgHash) const;
    /** The kept signed commitment for an epoch over `epochBlockHash`, under
     *  `msgHash` or, with no hash given, the first one kept for that base. The
     *  base is matched explicitly: a peer can deliver the commitment of the
     *  chain this node just switched to before the tick has moved the manager
     *  onto it, so one epoch can briefly hold entries for two bases, and the
     *  one kept first must not hide the one a block on this chain needs. */
    std::optional<CPoSeServiceCommitment> SignedCommitment(uint32_t nEpoch, const uint256& epochBlockHash,
                                                           const std::optional<uint256>& msgHash) const;

    static constexpr size_t kSignedCommitmentsPerEpoch{4};

private:
    mutable Mutex m_mutex;
    CServiceReportStore m_store;
    const uint32_t m_keepEpochs;
    uint32_t m_epoch GUARDED_BY(m_mutex){0};
    uint256 m_epochBlockHash GUARDED_BY(m_mutex);
    // per-epoch: which masternodes have announced liveness. Kept for the same
    // window as the store, so an announcement racing the local epoch tick in
    // either direction still lands in its own epoch's set.
    std::map<uint32_t, std::set<uint256>> m_responded GUARDED_BY(m_mutex);
    // per-epoch commitments, each under the hash the quorum signs. Only the
    // current epoch and the one before it are kept: a commitment is useful up
    // to its boundary block, and the previous epoch covers a producer whose
    // tick for the boundary has already run. A reorg drops what it invalidated
    // -- judged by the base block each commitment names, not by epoch number.
    struct EpochCommitments {
        std::vector<std::pair<uint256, CPoSeServiceCommitment>> candidates;
        std::vector<std::pair<uint256, CPoSeServiceCommitment>> signedCommitments;
    };
    std::map<uint32_t, EpochCommitments> m_commitments GUARDED_BY(m_mutex);

    // Drops every commitment older than the epoch before `nEpoch`. Called where
    // commitments are added, not only from BeginEpoch: a node that is not yet
    // blockchain-synced keeps connecting blocks -- and can keep receiving
    // commitments -- while ProcessDSLTick returns before BeginEpoch runs, so the
    // tick cannot be what keeps this map at two epochs (the same reasoning as the
    // global bound in HoldDSLEarlyResponse). Newer epochs are left alone: the wire
    // can deliver the next epoch's commitment before the tick reaches it.
    void PruneCommitmentsBefore(uint32_t nEpoch) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
};

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_MANAGER_H
