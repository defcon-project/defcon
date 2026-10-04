// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_sigcache.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_sentinels.h>
#include <hash.h>
#include <random.h>

#include <algorithm>
#include <mutex>

namespace dsl {
CReportSignatureCache::CReportSignatureCache(size_t entries)
    : m_salt(GetRandHash()), m_capacity(std::clamp<size_t>(entries, 2, MAX_ENTRIES))
{
    m_valid.setup(static_cast<uint32_t>(m_capacity));
}

bool CReportSignatureCache::Verify(const CPoSeServiceReport& report, const CBLSPublicKey& operatorKey,
                                   const uint256& epochBase)
{
    RecordPerf(report.nEpoch, epochBase, PerfMetric::VERIFY_REQUEST);
    // Invalid objects can serialize as zeros; never consult the cache for them.
    if (!report.sig.IsValid() || !operatorKey.IsValid()) {
        RecordPerf(report.nEpoch, epochBase, PerfMetric::VERIFY_INVALID);
        return false;
    }
    const uint256 sign_hash = report.GetSignHash(epochBase);
    CHashWriter writer(SER_GETHASH, 0);
    writer << std::string{"dsl-report-sigcache-v1"} << m_salt << uint8_t{0} << sign_hash;
    // Explicit modern scheme, independent of the mutable global BLS default.
    const auto key_bytes = operatorKey.ToBytes(/*specificLegacyScheme=*/false);
    const auto sig_bytes = report.sig.ToBytes(/*specificLegacyScheme=*/false);
    writer.write(MakeByteSpan(key_bytes));
    writer.write(MakeByteSpan(sig_bytes));
    const uint256 entry = writer.GetHash();
    bool cached;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        cached = m_valid.contains(entry, /*erase=*/false);
    }
    if (cached) {
        RecordPerf(report.nEpoch, epochBase, PerfMetric::CACHE_HIT);
        return true;
    }
    RecordPerf(report.nEpoch, epochBase, PerfMetric::CACHE_MISS);
    bool valid;
    {
        // No cache lock is held over BLS. Concurrent misses may recheck; no
        // inflight/negative entry can block a later valid report.
        PerfTimer bls_timer(report.nEpoch, epochBase, PerfMetric::BLS_VERIFY);
        valid = report.sig.VerifyInsecure(operatorKey, sign_hash, /*specificLegacyScheme=*/false);
    }
    if (valid) {
        RecordPerf(report.nEpoch, epochBase, PerfMetric::BLS_SUCCESS);
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_valid.insert(entry);
    }
    return valid;
}
} // namespace dsl
