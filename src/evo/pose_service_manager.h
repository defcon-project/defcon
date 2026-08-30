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
    explicit CPoSeServiceManager(uint32_t keepEpochs = 8) : m_store(keepEpochs), m_keepEpochs(keepEpochs) {}

    CServiceReportStore& Store() { return m_store; }
    const CServiceReportStore& Store() const { return m_store; }

    /**
     * Enter a new epoch: advance the store window and forget the previous
     * epoch's responses. Idempotent -- calling it again for the current epoch
     * does nothing, so a per-block tick can call it unconditionally.
     */
    void BeginEpoch(uint32_t nEpoch, const uint256& epochBlockHash);

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

    /**
     * Ingest a liveness announcement received from the flood. Accepts only an
     * announcement for the current epoch, from a masternode on the list, whose
     * signature verifies against that masternode's operator key -- so claiming
     * another node's identity fails on its key. On first sight the node is
     * recorded as responding and true is returned, telling the caller to relay
     * it onward; a duplicate or invalid announcement returns false.
     */
    bool ProcessResponse(const CPoSeServiceResponse& resp, const CDeterministicMNList& list);

    /**
     * Ingest a signed report received on the wire into the relay pool, using the
     * epoch-base hash this node recorded for the report's epoch. Returns whether
     * it was newly accepted -- the caller then relays it onward. A report for an
     * epoch this node never entered is refused.
     */
    bool ProcessReport(const CPoSeServiceReport& report, const CDeterministicMNList& list,
                       const Consensus::Params& params);

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

private:
    mutable Mutex m_mutex;
    CServiceReportStore m_store;
    const uint32_t m_keepEpochs;
    uint32_t m_epoch GUARDED_BY(m_mutex){0};
    uint256 m_epochBlockHash GUARDED_BY(m_mutex);
    std::set<uint256> m_responded GUARDED_BY(m_mutex);       // targets that answered this epoch
    std::map<uint32_t, uint256> m_epochHashes GUARDED_BY(m_mutex); // epoch -> base hash, within the window
};

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_MANAGER_H
