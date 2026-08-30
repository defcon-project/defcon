// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_manager.h>

#include <bls/bls.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>

#include <algorithm>

namespace dsl {

CPoSeServiceManager::EpochChange CPoSeServiceManager::BeginEpoch(uint32_t nEpoch,
                                                                const uint256& epochBlockHash)
{
    bool rebased = false;
    {
        LOCK(m_mutex);
        if (nEpoch == m_epoch && epochBlockHash == m_epochBlockHash) return EpochChange::None;
        // A reorg that keeps the epoch number but swaps its base block: the
        // responses gathered under the old base are stale and, worse, would
        // count each responder as "seen" and block its fresh announcement as a
        // duplicate. Drop them so the epoch is re-observed from the new base.
        rebased = (nEpoch == m_epoch) && (epochBlockHash != m_epochBlockHash);
        if (rebased) m_responded.erase(nEpoch);

        m_epoch = nEpoch;
        m_epochBlockHash = epochBlockHash;

        // drop announcement sets that have left the retained window
        const uint32_t oldest = nEpoch >= m_keepEpochs ? nEpoch - m_keepEpochs + 1 : 0;
        for (auto it = m_responded.begin(); it != m_responded.end();) {
            if (it->first < oldest) {
                it = m_responded.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (rebased) m_store.DropEpoch(nEpoch);
    m_store.SetCurrentEpoch(nEpoch);
    return rebased ? EpochChange::Rebased : EpochChange::Entered;
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
        if (const auto it = m_responded.find(m_epoch); it != m_responded.end()) responded = it->second;
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
    m_responded[m_epoch].insert(target);
}

CPoSeServiceResponse CPoSeServiceManager::AnnounceLiveness(const uint256& myProTxHash,
                                                           const CBLSSecretKey& myOperatorKey) const
{
    return AnnounceLiveness(myProTxHash, [&myOperatorKey](const uint256& hash) {
        return myOperatorKey.Sign(hash, /*specificLegacyScheme=*/false);
    });
}

CPoSeServiceResponse CPoSeServiceManager::AnnounceLiveness(const uint256& myProTxHash, const SignerFn& signer) const
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
    resp.sig = signer(ServiceChallengeNonce(epochHash, myProTxHash));
    return resp;
}

bool CPoSeServiceManager::ProcessResponse(const CPoSeServiceResponse& resp, const CDeterministicMNList& list,
                                          const uint256& epochBaseHash)
{
    {
        LOCK(m_mutex);
        // inside the retained window, in both directions -- the caller verified
        // the epoch's base block exists on its chain, this only bounds memory
        if (resp.nEpoch + m_keepEpochs <= m_epoch) return false;
        if (resp.nEpoch > m_epoch + m_keepEpochs) return false;
        const auto it = m_responded.find(resp.nEpoch);
        if (it != m_responded.end() && it->second.count(resp.proTxHash)) return false; // seen -- do not re-relay
    }
    const auto dmn = list.GetMN(resp.proTxHash);
    if (!dmn) return false;
    if (!VerifyChallengeResponse(resp.sig, dmn->pdmnState->pubKeyOperator.Get(), epochBaseHash, resp.proTxHash)) {
        return false;
    }
    LOCK(m_mutex);
    return m_responded[resp.nEpoch].insert(resp.proTxHash).second;
}

bool CPoSeServiceManager::ProcessReport(const CPoSeServiceReport& report, const CDeterministicMNList& list,
                                        const uint256& epochBaseHash, const Consensus::Params& params)
{
    return m_store.AddReport(report, list, epochBaseHash, params);
}

std::vector<CPoSeServiceReport> CPoSeServiceManager::EmitReports(const CDeterministicMNList& list,
                                                                const uint256& myProTxHash,
                                                                const CBLSSecretKey& myOperatorKey,
                                                                const Consensus::Params& params) const
{
    return EmitReports(list, myProTxHash, [&myOperatorKey](const uint256& hash) {
        return myOperatorKey.Sign(hash, /*specificLegacyScheme=*/false);
    }, params);
}

std::vector<CPoSeServiceReport> CPoSeServiceManager::EmitReports(const CDeterministicMNList& list,
                                                                const uint256& myProTxHash, const SignerFn& signer,
                                                                const Consensus::Params& params) const
{
    uint32_t epoch;
    uint256 epochHash;
    std::set<uint256> responded;
    {
        LOCK(m_mutex);
        epoch = m_epoch;
        epochHash = m_epochBlockHash;
        if (const auto it = m_responded.find(m_epoch); it != m_responded.end()) responded = it->second;
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
        r.sig = signer(r.GetSignHash(epochHash));
        out.push_back(std::move(r));
    }
    return out;
}

size_t CPoSeServiceManager::RespondedCount() const
{
    LOCK(m_mutex);
    const auto it = m_responded.find(m_epoch);
    return it == m_responded.end() ? 0 : it->second.size();
}

bool CPoSeServiceManager::HasResponded(const uint256& proTxHash) const
{
    LOCK(m_mutex);
    const auto it = m_responded.find(m_epoch);
    return it != m_responded.end() && it->second.count(proTxHash) > 0;
}

} // namespace dsl
