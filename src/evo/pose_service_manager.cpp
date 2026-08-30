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
