// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_manager.h>

#include <bls/bls.h>
#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <logging.h>

#include <algorithm>

namespace dsl {

CPoSeServiceManager::EpochChange CPoSeServiceManager::BeginEpoch(uint32_t nEpoch,
                                                                const uint256& epochBlockHash)
{
    EpochChange result = EpochChange::Entered;
    // Epochs whose store data a reorg invalidated. Collected under the lock but
    // dropped after it, matching how SetCurrentEpoch is called: the store has
    // its own synchronisation and must not be touched while m_mutex is held.
    std::vector<uint32_t> stale;
    {
        LOCK(m_mutex);
        if (nEpoch == m_epoch && epochBlockHash == m_epochBlockHash) return EpochChange::None;

        if (nEpoch < m_epoch) {
            // A reorg moved the tip back across a boundary. Every epoch from the
            // new one up to the old tip -- the new one included, since its base
            // block changed too -- was observed on a chain that no longer
            // exists. Drop all of them so nothing stale survives the rewind.
            for (auto it = m_responded.begin(); it != m_responded.end();) {
                if (it->first >= nEpoch) {
                    stale.push_back(it->first);
                    it = m_responded.erase(it);
                } else {
                    ++it;
                }
            }
            result = EpochChange::Rewound;
        } else if (nEpoch == m_epoch && epochBlockHash != m_epochBlockHash) {
            // Same epoch number, base block swapped by a shallow reorg: the
            // responses gathered under the old base would count each responder
            // as "seen" and block its fresh announcement as a duplicate.
            m_responded.erase(nEpoch);
            stale.push_back(nEpoch);
            result = EpochChange::Rebased;
        }
        // else nEpoch > m_epoch: ordinary forward progress, nothing is stale.

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

        // Commitments. Anything older than the epoch before this one has
        // passed its boundary block for good, and on a rewind every epoch above
        // the new one was on the abandoned chain. For the epoch itself, on a
        // rewind or a rebase, the base block decides and not the epoch number:
        // a commitment signed over the base this epoch still has is still the
        // one its boundary needs (a rewind that stops above the base), one
        // signed over a replaced base can enter no block on this chain.
        for (auto it = m_commitments.begin(); it != m_commitments.end();) {
            if (it->first + 1 < nEpoch || (result == EpochChange::Rewound && it->first > nEpoch)) {
                it = m_commitments.erase(it);
                continue;
            }
            if (it->first == nEpoch && (result == EpochChange::Rewound || result == EpochChange::Rebased)) {
                const auto other_base = [&](const auto& entry) { return entry.second.epochBlockHash != epochBlockHash; };
                auto& kept = it->second;
                kept.candidates.erase(std::remove_if(kept.candidates.begin(), kept.candidates.end(), other_base),
                                      kept.candidates.end());
                kept.signedCommitments.erase(
                    std::remove_if(kept.signedCommitments.begin(), kept.signedCommitments.end(), other_base),
                    kept.signedCommitments.end());
            }
            ++it;
        }
    }
    for (const uint32_t e : stale) m_store.DropEpoch(e);
    m_store.SetCurrentEpoch(nEpoch);
    LogPrint(BCLog::DSL, "DSL -- epoch %u %s, base %s%s\n", nEpoch,
             result == EpochChange::Rewound ? "rewound (reorg across a boundary)"
             : result == EpochChange::Rebased ? "rebased (base block replaced)"
                                              : "entered",
             epochBlockHash.ToString(),
             stale.empty() ? "" : strprintf(", %d stale epoch(s) dropped", stale.size()));
    return result;
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
        if (resp.nEpoch + m_keepEpochs <= m_epoch) {
            LogPrint(BCLog::DSL, "DSL -- announcement by %s for epoch %u refused: aged out (current epoch %u)\n",
                     resp.proTxHash.ToString(), resp.nEpoch, m_epoch);
            return false;
        }
        if (resp.nEpoch > m_epoch + m_keepEpochs) {
            LogPrint(BCLog::DSL, "DSL -- announcement by %s for epoch %u refused: beyond the window (current epoch %u)\n",
                     resp.proTxHash.ToString(), resp.nEpoch, m_epoch);
            return false;
        }
        const auto it = m_responded.find(resp.nEpoch);
        // seen -- do not re-relay. Not logged: on a gossip mesh every announcement
        // arrives many times over, and a line per copy would drown the rest.
        if (it != m_responded.end() && it->second.count(resp.proTxHash)) return false;
    }
    const auto dmn = list.GetMN(resp.proTxHash);
    if (!dmn) {
        LogPrint(BCLog::DSL, "DSL -- announcement by %s for epoch %u refused: not a masternode of the epoch's list\n",
                 resp.proTxHash.ToString(), resp.nEpoch);
        return false;
    }
    if (!VerifyChallengeResponse(resp.sig, dmn->pdmnState->pubKeyOperator.Get(), epochBaseHash, resp.proTxHash)) {
        LogPrint(BCLog::DSL, "DSL -- announcement by %s for epoch %u refused: bad signature\n",
                 resp.proTxHash.ToString(), resp.nEpoch);
        return false;
    }
    LOCK(m_mutex);
    auto& responded = m_responded[resp.nEpoch];
    const bool fresh = responded.insert(resp.proTxHash).second;
    if (fresh) {
        LogPrint(BCLog::DSL, "DSL -- announcement by %s for epoch %u accepted (%d responded so far)\n",
                 resp.proTxHash.ToString(), resp.nEpoch, responded.size());
    }
    return fresh;
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
    const auto missed = std::count_if(out.begin(), out.end(), [](const CPoSeServiceReport& r) {
        return r.status == static_cast<uint8_t>(ServiceStatus::MISSED);
    });
    LogPrint(BCLog::DSL, "DSL -- epoch %u: %d sentinel report(s) emitted as %s, %d marked missed\n", epoch, out.size(),
             myProTxHash.ToString(), missed);
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

namespace {
template <typename Entries>
auto FindCommitment(Entries& entries, const uint256& msgHash)
{
    return std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.first == msgHash; });
}
} // namespace

void CPoSeServiceManager::PruneCommitmentsBefore(uint32_t nEpoch)
{
    AssertLockHeld(m_mutex);
    for (auto it = m_commitments.begin(); it != m_commitments.end() && it->first + 1 < nEpoch;) {
        it = m_commitments.erase(it);
    }
}

void CPoSeServiceManager::RememberSigningCandidate(const CPoSeServiceCommitment& commitment, const uint256& msgHash)
{
    LOCK(m_mutex);
    // An epoch that already left the window cannot be signed for any more.
    if (commitment.nEpoch + 1 < m_epoch) return;
    PruneCommitmentsBefore(commitment.nEpoch);
    auto& candidates = m_commitments[commitment.nEpoch].candidates;
    if (FindCommitment(candidates, msgHash) != candidates.end()) return;
    if (candidates.size() >= kSignedCommitmentsPerEpoch) return;
    candidates.emplace_back(msgHash, commitment);
}

std::optional<CPoSeServiceCommitment> CPoSeServiceManager::SigningCandidate(uint32_t nEpoch, const uint256& msgHash) const
{
    LOCK(m_mutex);
    const auto epoch = m_commitments.find(nEpoch);
    if (epoch == m_commitments.end()) return std::nullopt;
    const auto it = FindCommitment(epoch->second.candidates, msgHash);
    if (it == epoch->second.candidates.end()) return std::nullopt;
    return it->second;
}

bool CPoSeServiceManager::StoreSignedCommitment(const CPoSeServiceCommitment& commitment, const uint256& msgHash)
{
    LOCK(m_mutex);
    if (commitment.nEpoch + 1 < m_epoch) return false;
    PruneCommitmentsBefore(commitment.nEpoch);
    auto& kept = m_commitments[commitment.nEpoch].signedCommitments;
    if (FindCommitment(kept, msgHash) != kept.end()) return false;
    if (kept.size() >= kSignedCommitmentsPerEpoch) {
        LogPrint(BCLog::DSL, "DSL -- signed commitment for epoch %u not kept: %d already held for that epoch\n",
                 commitment.nEpoch, kept.size());
        return false;
    }
    kept.emplace_back(msgHash, commitment);
    return true;
}

bool CPoSeServiceManager::HasSignedCommitment(uint32_t nEpoch, const uint256& msgHash) const
{
    LOCK(m_mutex);
    const auto epoch = m_commitments.find(nEpoch);
    return epoch != m_commitments.end() &&
           FindCommitment(epoch->second.signedCommitments, msgHash) != epoch->second.signedCommitments.end();
}

std::optional<CPoSeServiceCommitment> CPoSeServiceManager::SignedCommitment(uint32_t nEpoch, const uint256& epochBlockHash,
                                                                            const std::optional<uint256>& msgHash) const
{
    LOCK(m_mutex);
    const auto epoch = m_commitments.find(nEpoch);
    if (epoch == m_commitments.end()) return std::nullopt;
    for (const auto& [hash, commitment] : epoch->second.signedCommitments) {
        if (commitment.epochBlockHash != epochBlockHash) continue;
        if (msgHash.has_value() && hash != *msgHash) continue;
        return commitment;
    }
    return std::nullopt;
}

} // namespace dsl
