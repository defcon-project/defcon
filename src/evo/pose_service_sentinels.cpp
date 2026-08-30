// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_sentinels.h>

#include <evo/deterministicmns.h>
#include <hash.h>

#include <algorithm>
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

} // namespace dsl
