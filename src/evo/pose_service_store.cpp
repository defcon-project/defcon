// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_store.h>

#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <logging.h>

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

    // Every refusal below names its reason under the dsl category, except the
    // duplicate: on a gossip mesh each report arrives many times over, and a
    // line per copy would drown the rest.
    const auto refuse = [&](const char* why) {
        LogPrint(BCLog::DSL, "DSL -- report by %s on %s for epoch %u refused: %s\n",
                 report.sentinelProTxHash.ToString(), report.targetProTxHash.ToString(), report.nEpoch, why);
        return false;
    };

    // Inside the retained epoch window: never a future epoch, never one that has
    // already aged out.
    if (report.nEpoch > m_currentEpoch) return refuse("future epoch");
    if (report.nEpoch < OldestKeptEpoch()) return refuse("epoch aged out");

    const Key key{report.nEpoch, report.targetProTxHash, report.sentinelProTxHash};
    if (m_reports.count(key)) return false; // one report per sentinel per target per epoch

    // Both ends have to be masternodes the epoch knew about, and the target is
    // the one that bounds this store. The assignment below is derived FROM the
    // target hash, so every 256-bit value has a sentinel set of its own --
    // including values that name no masternode at all. Without this check an
    // operator could grind hashes until its own node fell in the set and mint a
    // distinct, correctly signed report for each one; every accepted report is
    // held until its epoch ages out and flooded to every peer, and the map has
    // no bound of its own. With it, a key can file only the handful of reports
    // the epoch actually assigned it. The verdict was never at risk -- the
    // aggregation walks the canonical list, so a report about a target that
    // does not exist reaches no bit -- the memory and the bandwidth were.
    const auto tdmn = epochList.GetMN(report.targetProTxHash);
    if (!tdmn) return refuse("target is not a masternode of the epoch's list");
    const auto sdmn = epochList.GetMN(report.sentinelProTxHash);
    if (!sdmn) return refuse("sentinel is not a masternode of the epoch's list");

    // The sentinel must be one this epoch assigned to the target -- a node that
    // was never asked to probe this target cannot contribute an observation.
    //
    // This check used to come second, on the reasoning that verifying a signature
    // costs the same at any network size while deriving the assignment sorts the
    // whole list, so the cheap test should go first. That holds only against an
    // attacker who cannot sign. A registered masternode signs with its own
    // operator key, so it passes the signature check on every report it cares to
    // invent, naming itself sentinel for every target in the list; each one then
    // paid a full derivation while holding m_mutex. Memoised, assignment is a
    // lookup after the first report for a target -- so it is now both the cheaper
    // test and the only one that stops the case the other cannot.
    const auto& sentinels = SentinelsFor(epochList, report.targetProTxHash, epochBlockHash,
                                         static_cast<size_t>(params.nDSLSentinelCount));
    if (std::find(sentinels.begin(), sentinels.end(), report.sentinelProTxHash) == sentinels.end()) {
        return refuse("sentinel was not assigned to this target");
    }

    if (!report.VerifySig(sdmn->pdmnState->pubKeyOperator.Get(), epochBlockHash)) return refuse("bad signature");

    m_reports.emplace(key, report);
    LogPrint(BCLog::DSL, "DSL -- report by %s on %s for epoch %u kept: %s (%d reports held)\n",
             report.sentinelProTxHash.ToString(), report.targetProTxHash.ToString(), report.nEpoch,
             report.status == static_cast<uint8_t>(ServiceStatus::MISSED) ? "missed" : "online", m_reports.size());
    return true;
}

const std::vector<uint256>& CServiceReportStore::SentinelsFor(const CDeterministicMNList& epochList,
                                                              const uint256& targetProTxHash,
                                                              const uint256& epochBlockHash,
                                                              size_t count)
{
    AssertLockHeld(m_mutex);
    if (m_assignBase != epochBlockHash) {
        m_assignBase = epochBlockHash;
        m_assignCache.clear();
    }
    if (const auto it = m_assignCache.find(targetProTxHash); it != m_assignCache.end()) {
        return it->second;
    }
    return m_assignCache
        .emplace(targetProTxHash,
                 CalcSentinelsForMN(epochList, targetProTxHash, epochBlockHash, count))
        .first->second;
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
    size_t dropped{0};
    for (auto it = m_reports.begin(); it != m_reports.end();) {
        if (std::get<0>(it->first) == nEpoch) {
            it = m_reports.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    LogPrint(BCLog::DSL, "DSL -- epoch %u dropped from the report store: %d report(s) discarded\n", nEpoch, dropped);
}

size_t CServiceReportStore::Size() const
{
    LOCK(m_mutex);
    return m_reports.size();
}

} // namespace dsl
