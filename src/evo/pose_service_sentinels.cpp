// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_sentinels.h>

#include <evo/deterministicmns.h>
#include <evo/pose_service.h>
#include <evo/specialtx.h>
#include <hash.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace dsl {

std::vector<uint256> CalcSentinelsForMN(const CDeterministicMNList& list,
                                        const uint256& targetProTxHash,
                                        const uint256& epochBlockHash, size_t count)
{
    CHashWriter w(SER_GETHASH, 0);
    w << targetProTxHash << epochBlockHash;
    const uint256 modifier = w.GetHash();

    auto scores = list.CalculateScores(modifier);
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<uint256> out;
    out.reserve(count);
    for (const auto& s : scores) {
        if (s.second->proTxHash == targetProTxHash) continue;
        out.push_back(s.second->proTxHash);
        if (out.size() == count) break;
    }
    return out;
}

uint256 ServiceChallengeNonce(const uint256& epochBlockHash, const uint256& targetProTxHash)
{
    CHashWriter w(SER_GETHASH, 0);
    w << std::string{"dslchallenge"} << epochBlockHash << targetProTxHash;
    return w.GetHash();
}

std::vector<uint256> GetProbeTargetsForSentinel(const CDeterministicMNList& list,
                                                const uint256& sentinelProTxHash,
                                                const uint256& epochBlockHash, size_t count)
{
    std::vector<uint256> out;
    list.ForEachMN(false, [&](const auto& dmn) {
        if (dmn.proTxHash == sentinelProTxHash) return;
        const auto sentinels = CalcSentinelsForMN(list, dmn.proTxHash, epochBlockHash, count);
        if (std::find(sentinels.begin(), sentinels.end(), sentinelProTxHash) != sentinels.end()) {
            out.push_back(dmn.proTxHash);
        }
    });
    return out;
}

CBLSSignature SignChallengeResponse(const CBLSSecretKey& targetOperatorKey,
                                    const uint256& epochBlockHash, const uint256& targetProTxHash)
{
    return targetOperatorKey.Sign(ServiceChallengeNonce(epochBlockHash, targetProTxHash),
                                  /*specificLegacyScheme=*/false);
}

bool VerifyChallengeResponse(const CBLSSignature& sig, const CBLSPublicKey& targetOperatorKey,
                             const uint256& epochBlockHash, const uint256& targetProTxHash)
{
    return sig.VerifyInsecure(targetOperatorKey, ServiceChallengeNonce(epochBlockHash, targetProTxHash),
                              /*specificLegacyScheme=*/false);
}

uint256 CPoSeServiceReport::GetSignHash() const
{
    CHashWriter w(SER_GETHASH, 0);
    w << std::string{"dslreport"} << nEpoch << targetProTxHash << sentinelProTxHash << status;
    return w.GetHash();
}

void CPoSeServiceReport::Sign(const CBLSSecretKey& operatorKey)
{
    sig = operatorKey.Sign(GetSignHash(), /*specificLegacyScheme=*/false);
}

bool CPoSeServiceReport::VerifySig(const CBLSPublicKey& operatorPubKey) const
{
    return sig.VerifyInsecure(operatorPubKey, GetSignHash(), /*specificLegacyScheme=*/false);
}

CPoSeServiceCommitment BuildServiceCommitment(uint32_t nEpoch, const uint256& epochBlockHash,
                                              Consensus::LLMQType llmqType, const uint256& quorumHash,
                                              const std::vector<CPoSeServiceReport>& reports,
                                              const CDeterministicMNList& epochBaseList,
                                              const Consensus::Params& params)
{
    // canonical order the bitfield indexes
    std::vector<uint256> order;
    order.reserve(epochBaseList.GetAllMNsCount());
    epochBaseList.ForEachMN(false, [&](const auto& dmn) { order.push_back(dmn.proTxHash); });
    std::sort(order.begin(), order.end());

    std::map<uint256, std::vector<const CPoSeServiceReport*>> byTarget;
    for (const auto& r : reports) {
        if (r.nEpoch == nEpoch) byTarget[r.targetProTxHash].push_back(&r);
    }

    CPoSeServiceCommitment c;
    c.nEpoch = nEpoch;
    c.epochBlockHash = epochBlockHash;
    c.llmqType = llmqType;
    c.quorumHash = quorumHash;
    c.missed.assign(order.size(), false);

    for (size_t i = 0; i < order.size(); ++i) {
        const auto sentinels = CalcSentinelsForMN(epochBaseList, order[i], epochBlockHash,
                                                  static_cast<size_t>(params.nDSLSentinelCount));
        const std::set<uint256> assigned(sentinels.begin(), sentinels.end());
        std::set<uint256> counted;
        size_t missedCount = 0;
        auto it = byTarget.find(order[i]);
        if (it != byTarget.end()) {
            for (const auto* r : it->second) {
                if (!assigned.count(r->sentinelProTxHash)) continue;      // not an assigned sentinel
                if (!counted.insert(r->sentinelProTxHash).second) continue; // one report per sentinel
                const auto sdmn = epochBaseList.GetMN(r->sentinelProTxHash);
                if (!sdmn) continue;
                if (!r->VerifySig(sdmn->pdmnState->pubKeyOperator.Get())) continue;
                if (r->status == static_cast<uint8_t>(ServiceStatus::MISSED)) ++missedCount;
            }
        }
        if (static_cast<int>(missedCount) >= params.nDSLSentinelAgree) {
            c.missed[i] = true;
        }
    }
    return c;
}

ServiceCommitmentTxCandidate BuildServiceCommitmentTx(uint32_t nEpoch, const uint256& epochBlockHash,
                                                      Consensus::LLMQType llmqType, const uint256& quorumHash,
                                                      const std::vector<CPoSeServiceReport>& reports,
                                                      const CDeterministicMNList& epochBaseList,
                                                      const Consensus::Params& params)
{
    ServiceCommitmentTxCandidate out;
    out.commitment = BuildServiceCommitment(nEpoch, epochBlockHash, llmqType, quorumHash,
                                            reports, epochBaseList, params);

    CPoSeServiceCommitmentTxPayload payload;
    payload.commitment = out.commitment;

    out.tx.nVersion = 3;
    out.tx.nType = TRANSACTION_POSE_SERVICE_COMMITMENT;
    SetTxPayload(out.tx, payload);
    out.msgHash = out.tx.GetHash();
    return out;
}

} // namespace dsl
