// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_store.h>

#include <consensus/params.h>
#include <evo/deterministicmns.h>

#include <algorithm>

namespace dsl {

uint32_t CServiceReportStore::OldestKeptEpoch() const
{
    AssertLockHeld(m_mutex);
    if (m_currentEpoch < m_keepEpochs) return 0;
    return m_currentEpoch - m_keepEpochs + 1;
}

bool CServiceReportStore::AddReport(const CPoSeServiceReport& report, const CDeterministicMNList& epochList,
                                    const uint256& epochBlockHash, const Consensus::Params& params)
{
    LOCK(m_mutex);

    // Inside the retained epoch window: never a future epoch, never one that has
    // already aged out.
    if (report.nEpoch > m_currentEpoch) return false;
    if (report.nEpoch < OldestKeptEpoch()) return false;

    const Key key{report.nEpoch, report.targetProTxHash, report.sentinelProTxHash};
    if (m_reports.count(key)) return false; // one report per sentinel per target per epoch

    // The sentinel must be one this epoch assigned to the target -- a node that
    // was never asked to probe this target cannot contribute an observation.
    const auto sentinels = CalcSentinelsForMN(epochList, report.targetProTxHash, epochBlockHash,
                                              static_cast<size_t>(params.nDSLSentinelCount));
    if (std::find(sentinels.begin(), sentinels.end(), report.sentinelProTxHash) == sentinels.end()) {
        return false;
    }

    // And the signature must verify against that sentinel's operator key.
    const auto sdmn = epochList.GetMN(report.sentinelProTxHash);
    if (!sdmn) return false;
    if (!report.VerifySig(sdmn->pdmnState->pubKeyOperator.Get())) return false;

    m_reports.emplace(key, report);
    return true;
}

std::vector<CPoSeServiceReport> CServiceReportStore::GetReportsForEpoch(uint32_t nEpoch) const
{
    LOCK(m_mutex);
    std::vector<CPoSeServiceReport> out;
    for (const auto& [key, report] : m_reports) {
        if (std::get<0>(key) == nEpoch) out.push_back(report);
    }
    return out;
}

bool CServiceReportStore::HaveReport(uint32_t nEpoch, const uint256& target, const uint256& sentinel) const
{
    LOCK(m_mutex);
    return m_reports.count(Key{nEpoch, target, sentinel}) > 0;
}

void CServiceReportStore::SetCurrentEpoch(uint32_t nEpoch)
{
    LOCK(m_mutex);
    m_currentEpoch = nEpoch;
    const uint32_t oldest = OldestKeptEpoch();
    for (auto it = m_reports.begin(); it != m_reports.end();) {
        if (std::get<0>(it->first) < oldest) {
            it = m_reports.erase(it);
        } else {
            ++it;
        }
    }
}

void CServiceReportStore::DropEpoch(uint32_t nEpoch)
{
    LOCK(m_mutex);
    for (auto it = m_reports.begin(); it != m_reports.end();) {
        if (std::get<0>(it->first) == nEpoch) {
            it = m_reports.erase(it);
        } else {
            ++it;
        }
    }
}

size_t CServiceReportStore::Size() const
{
    LOCK(m_mutex);
    return m_reports.size();
}

} // namespace dsl
