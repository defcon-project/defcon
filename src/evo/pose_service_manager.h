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
#include <set>
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
};

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_MANAGER_H
