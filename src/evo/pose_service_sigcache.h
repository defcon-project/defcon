// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_SIGCACHE_H
#define BITCOIN_EVO_POSE_SERVICE_SIGCACHE_H

#include <bls/bls.h>
#include <cuckoocache.h>
#include <uint256.h>
#include <util/hasher.h>

#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace dsl {
class CPoSeServiceReport;

/**
 * Who asks the cache, for its counters and its log only; the answer never depends on it. Set for the calling
 * thread by ScopedVerifyContext.
 *  - PREVERIFY: the report pre-verifier's thread (ReportPreverifier). Its checks count as preverify_bls, not as
 *    verify_request, cache_hit, cache_miss, bls_verify or bls_success, and each miss, insert and hit is logged
 *    under the net category with the entry's id.
 *  - NET_ADMIT: the message thread admitting a report off the network. Counted as anywhere else, and a miss also
 *    as net_admit_cache_miss; each hit and miss is logged with the entry's id.
 */
enum class VerifyContext : uint8_t { DEFAULT, PREVERIFY, NET_ADMIT };
class ScopedVerifyContext
{
public:
    explicit ScopedVerifyContext(VerifyContext context);
    ~ScopedVerifyContext();
    ScopedVerifyContext(const ScopedVerifyContext&) = delete;
    ScopedVerifyContext& operator=(const ScopedVerifyContext&) = delete;

private:
    const VerifyContext m_previous;
};

/** Tests only: a function CReportSignatureCache::Verify calls on its own thread, with the caller's context -- with
 *  `inserted` false inside the BLS check of an entry it missed, before the BLS, while it holds no lock of the cache; with
 *  `inserted` true once a valid entry is in the cache and its lock is released. nullptr, the default, for none. */
using SigcacheTestHook = void (*)(VerifyContext context, bool inserted);
void SetSigcacheTestHook(SigcacheTestHook hook);

// Cryptographic facts only. Assignment, duplicate and epoch checks remain at
// every caller. Failed checks are never inserted. Independent of ECDSA caches.
class CReportSignatureCache {
public:
    static constexpr size_t MAX_ENTRIES{16384};
    explicit CReportSignatureCache(size_t entries = MAX_ENTRIES);
    bool Verify(const CPoSeServiceReport& report, const CBLSPublicKey& operatorKey,
                const uint256& epochBase);
    size_t Capacity() const { return m_capacity; }
    //! Forgets every entry (tests only: a forced miss): a new salt, so that no earlier entry is found again. The
    //! cuckoo cache itself has no way to empty itself (contains() does not read its collection flags).
    void Clear();
private:
    CuckooCache::cache<uint256, SignatureCacheHasher> m_valid;
    uint256 m_salt; // guarded by m_mutex: Clear() replaces it
    const size_t m_capacity;
    std::shared_mutex m_mutex;
};
} // namespace dsl
#endif // BITCOIN_EVO_POSE_SERVICE_SIGCACHE_H
