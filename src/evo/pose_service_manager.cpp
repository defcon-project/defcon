// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_manager.h>

#include <bls/bls.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>

#include <algorithm>

namespace dsl {

void CPoSeServiceManager::BeginEpoch(uint32_t nEpoch, const uint256& epochBlockHash)
{
    {
        LOCK(m_mutex);
        if (nEpoch == m_epoch && epochBlockHash == m_epochBlockHash) return;
        m_epoch = nEpoch;
        m_epochBlockHash = epochBlockHash;
        m_responded.clear();

        // Remember this epoch's base hash so a slightly-late report for it can
        // still be validated, and drop hashes that have left the window.
        m_epochHashes[nEpoch] = epochBlockHash;
        const uint32_t oldest = nEpoch >= m_keepEpochs ? nEpoch - m_keepEpochs + 1 : 0;
        for (auto it = m_epochHashes.begin(); it != m_epochHashes.end();) {
            if (it->first < oldest) {
                it = m_epochHashes.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_store.SetCurrentEpoch(nEpoch);
}

uint32_t CPoSeServiceManager::CurrentEpoch() const
{
    LOCK(m_mutex);
    return m_epoch;
}

uint256 CPoSeServiceManager::CurrentEpochHash() const
{
    LOCK(m_mutex);
    return m_epochBlockHash;
}

std::vector<uint256> CPoSeServiceManager::PendingChallenges(const CDeterministicMNList& list,
                                                            const uint256& myProTxHash,
                                                            const Consensus::Params& params) const
{
    uint256 epochHash;
    std::set<uint256> responded;
    {
        LOCK(m_mutex);
        epochHash = m_epochBlockHash;
        responded = m_responded;
    }
    auto targets = GetProbeTargetsForSentinel(list, myProTxHash, epochHash,
                                              static_cast<size_t>(params.nDSLSentinelCount));
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [&](const uint256& t) { return responded.count(t) > 0; }),
                  targets.end());
    return targets;
}

void CPoSeServiceManager::RecordResponse(const uint256& target)
{
    LOCK(m_mutex);
    m_responded.insert(target);
}

CPoSeServiceResponse CPoSeServiceManager::AnnounceLiveness(const uint256& myProTxHash,
                                                           const CBLSSecretKey& myOperatorKey) const
{
    uint32_t epoch;
    uint256 epochHash;
    {
        LOCK(m_mutex);
        epoch = m_epoch;
        epochHash = m_epochBlockHash;
    }
    CPoSeServiceResponse resp;
    resp.nEpoch = epoch;
    resp.proTxHash = myProTxHash;
    resp.sig = SignChallengeResponse(myOperatorKey, epochHash, myProTxHash);
    return resp;
}

bool CPoSeServiceManager::ProcessResponse(const CPoSeServiceResponse& resp, const CDeterministicMNList& list)
{
    uint32_t epoch;
    uint256 epochHash;
    {
        LOCK(m_mutex);
        epoch = m_epoch;
        epochHash = m_epochBlockHash;
        if (resp.nEpoch != epoch) return false;
        if (m_responded.count(resp.proTxHash)) return false; // seen already -- do not re-relay
    }
    const auto dmn = list.GetMN(resp.proTxHash);
    if (!dmn) return false;
    if (!VerifyChallengeResponse(resp.sig, dmn->pdmnState->pubKeyOperator.Get(), epochHash, resp.proTxHash)) {
        return false;
    }
    LOCK(m_mutex);
    if (resp.nEpoch != m_epoch) return false; // epoch rolled while verifying
    return m_responded.insert(resp.proTxHash).second;
}

bool CPoSeServiceManager::ProcessReport(const CPoSeServiceReport& report, const CDeterministicMNList& list,
                                        const Consensus::Params& params)
{
    uint256 epochHash;
    {
        LOCK(m_mutex);
        const auto it = m_epochHashes.find(report.nEpoch);
        if (it == m_epochHashes.end()) return false; // an epoch this node never entered
        epochHash = it->second;
    }
    return m_store.AddReport(report, list, epochHash, params);
}

std::vector<CPoSeServiceReport> CPoSeServiceManager::EmitReports(const CDeterministicMNList& list,
                                                                const uint256& myProTxHash,
                                                                const CBLSSecretKey& myOperatorKey,
                                                                const Consensus::Params& params) const
{
    uint32_t epoch;
    uint256 epochHash;
    std::set<uint256> responded;
    {
        LOCK(m_mutex);
        epoch = m_epoch;
        epochHash = m_epochBlockHash;
        responded = m_responded;
    }
    const auto targets = GetProbeTargetsForSentinel(list, myProTxHash, epochHash,
                                                    static_cast<size_t>(params.nDSLSentinelCount));
    std::vector<CPoSeServiceReport> out;
    out.reserve(targets.size());
    for (const auto& t : targets) {
        CPoSeServiceReport r;
        r.nEpoch = epoch;
        r.targetProTxHash = t;
        r.sentinelProTxHash = myProTxHash;
        r.status = static_cast<uint8_t>(responded.count(t) ? ServiceStatus::ONLINE : ServiceStatus::MISSED);
        r.Sign(myOperatorKey);
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace dsl
