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
#include <shared_mutex>

namespace dsl {
class CPoSeServiceReport;

// Cryptographic facts only. Assignment, duplicate and epoch checks remain at
// every caller. Failed checks are never inserted. Independent of ECDSA caches.
class CReportSignatureCache {
public:
    static constexpr size_t MAX_ENTRIES{16384};
    explicit CReportSignatureCache(size_t entries = MAX_ENTRIES);
    bool Verify(const CPoSeServiceReport& report, const CBLSPublicKey& operatorKey,
                const uint256& epochBase);
    size_t Capacity() const { return m_capacity; }
private:
    CuckooCache::cache<uint256, SignatureCacheHasher> m_valid;
    const uint256 m_salt;
    const size_t m_capacity;
    std::shared_mutex m_mutex;
};
} // namespace dsl
#endif
