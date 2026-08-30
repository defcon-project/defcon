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
 * challenge, records the answers, and emits this node's signed reports at the
 * epoch cutoff -- but holds no sockets itself, so it is exercised in isolation.
 *
 * One epoch is live at a time. BeginEpoch rolls the window forward and clears
 * the previous epoch's answers; RecordResponse notes a verified liveness proof;
 * EmitReports turns "probed and answered" into ONLINE and "probed and silent"
 * into MISSED. The store it owns is the relay pool the aggregator later reads.
 */
class CPoSeServiceManager
{
public:
    explicit CPoSeServiceManager(uint32_t keepEpochs = 8) : m_store(keepEpochs) {}

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
    uint32_t m_epoch GUARDED_BY(m_mutex){0};
    uint256 m_epochBlockHash GUARDED_BY(m_mutex);
    std::set<uint256> m_responded GUARDED_BY(m_mutex); // targets that answered this epoch
};

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_MANAGER_H
