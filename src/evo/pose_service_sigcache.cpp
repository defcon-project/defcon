// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_sigcache.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_sentinels.h>
#include <hash.h>
#include <logging.h>
#include <random.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace dsl {
namespace {
thread_local VerifyContext verify_context{VerifyContext::DEFAULT};
std::atomic<SigcacheTestHook> g_test_hook{nullptr};

//! The short id of a cache entry that the log lines of one report share.
std::string EntryId(const uint256& entry) { return entry.ToString().substr(0, 16); }

//! The BLS check of a report the cache missed. The test hook is inside it, so whatever would surround the check
//! surrounds the hook as well (a test stops the dslverify thread there).
bool CheckSignature(const CPoSeServiceReport& report, const CBLSPublicKey& operatorKey, const uint256& sign_hash,
                    VerifyContext context)
{
    if (const auto hook = g_test_hook.load()) hook(context, /*inserted=*/false);
    return report.sig.VerifyInsecure(operatorKey, sign_hash, /*specificLegacyScheme=*/false);
}
} // namespace

ScopedVerifyContext::ScopedVerifyContext(VerifyContext context) : m_previous(verify_context)
{
    verify_context = context;
}
ScopedVerifyContext::~ScopedVerifyContext() { verify_context = m_previous; }

void SetSigcacheTestHook(SigcacheTestHook hook) { g_test_hook = hook; }

CReportSignatureCache::CReportSignatureCache(size_t entries)
    : m_salt(GetRandHash()), m_capacity(std::clamp<size_t>(entries, 2, MAX_ENTRIES))
{
    m_valid.setup(static_cast<uint32_t>(m_capacity));
}

void CReportSignatureCache::Clear()
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_salt = GetRandHash();
}

bool CReportSignatureCache::Verify(const CPoSeServiceReport& report, const CBLSPublicKey& operatorKey,
                                   const uint256& epochBase)
{
    const VerifyContext context{verify_context};
    const bool preverify{context == VerifyContext::PREVERIFY};
    const auto count = [&](PerfMetric metric) {
        if (!preverify) RecordPerf(report.nEpoch, epochBase, metric);
    };
    count(PerfMetric::VERIFY_REQUEST);
    // Invalid objects can serialize as zeros; never consult the cache for them.
    if (!report.sig.IsValid() || !operatorKey.IsValid()) {
        count(PerfMetric::VERIFY_INVALID);
        return false;
    }
    const uint256 sign_hash = report.GetSignHash(epochBase);
    const uint256 salt = [&] {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_salt;
    }();
    CHashWriter writer(SER_GETHASH, 0);
    writer << std::string{"dsl-report-sigcache-v1"} << salt << uint8_t{0} << sign_hash;
    // Explicit modern scheme, independent of the mutable global BLS default.
    const auto key_bytes = operatorKey.ToBytes(/*specificLegacyScheme=*/false);
    const auto sig_bytes = report.sig.ToBytes(/*specificLegacyScheme=*/false);
    writer.write(MakeByteSpan(key_bytes));
    writer.write(MakeByteSpan(sig_bytes));
    const uint256 entry = writer.GetHash();
    const char* who{preverify ? "preverify" : context == VerifyContext::NET_ADMIT ? "net admit" : nullptr};
    bool cached;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        cached = m_valid.contains(entry, /*erase=*/false);
    }
    if (cached) {
        count(PerfMetric::CACHE_HIT);
        if (who != nullptr) LogPrint(BCLog::NET, "DSL sigcache -- %s hit entry=%s\n", who, EntryId(entry));
        return true;
    }
    count(PerfMetric::CACHE_MISS);
    if (context == VerifyContext::NET_ADMIT) RecordPerf(report.nEpoch, epochBase, PerfMetric::NET_ADMIT_CACHE_MISS);
    if (who != nullptr) LogPrint(BCLog::NET, "DSL sigcache -- %s miss entry=%s\n", who, EntryId(entry));
    bool valid;
    {
        // No cache lock is held over BLS. Concurrent misses may recheck; no
        // inflight/negative entry can block a later valid report.
        PerfTimer bls_timer(report.nEpoch, epochBase, preverify ? PerfMetric::PREVERIFY_BLS : PerfMetric::BLS_VERIFY);
        valid = CheckSignature(report, operatorKey, sign_hash, context);
    }
    if (valid) {
        count(PerfMetric::BLS_SUCCESS);
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            // Logged before the entry can be found, so that a hit on it is never logged ahead of its insert.
            if (preverify) LogPrint(BCLog::NET, "DSL sigcache -- preverify insert entry=%s\n", EntryId(entry));
            m_valid.insert(entry);
        }
        if (const auto hook = g_test_hook.load()) hook(context, /*inserted=*/true);
    }
    return valid;
}
} // namespace dsl
