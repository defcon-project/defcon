// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_STORE_H
#define BITCOIN_EVO_POSE_SERVICE_STORE_H

#include <evo/pose_service_sentinels.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

class CDeterministicMNList;
namespace Consensus {
struct Params;
}

namespace dsl {

/**
 * An in-memory, spam-resistant store of the signed service reports a node has
 * seen for the current epoch and a few before it.
 *
 * It is a relay filter, not the authority. It accepts a report only when the
 * report is signed by a masternode's operator key and its sentinel is one the
 * epoch actually assigned to the target -- so a peer cannot flood junk, and
 * cannot inject observations from nodes that were never asked to probe. But the
 * on-chain verdict is recomputed from the canonical list by
 * BuildServiceCommitment, which re-checks assignment and signatures itself, so a
 * report the store accepted a little too liberally (for instance across a reorg
 * that moved the epoch base) never becomes a punishment on its own. That
 * layering is deliberate: the store is cheap gossip hygiene; the aggregator is
 * consensus.
 *
 * Reports are held per epoch and dropped once their epoch falls outside the
 * retained window, which is advanced by SetCurrentEpoch.
 */
class CServiceReportStore
{
public:
    explicit CServiceReportStore(uint32_t keepEpochs = 8) : m_keepEpochs(keepEpochs) {}

    /**
     * Validate and store one report. Returns true when the report is newly
     * accepted -- the caller then relays it onward. Rejects a report for a
     * future epoch or one older than the retained window, a duplicate of an
     * (epoch, target, sentinel) already held, a sentinel the epoch did not
     * assign to the target, or a signature that does not verify against that
     * sentinel's operator key.
     */
    bool AddReport(const CPoSeServiceReport& report, const CDeterministicMNList& epochList,
                   const uint256& epochBlockHash, const Consensus::Params& params);

    /** Every accepted report for one epoch, for the aggregator to build a bitfield. */
    std::vector<CPoSeServiceReport> GetReportsForEpoch(uint32_t nEpoch) const;

    /** True if a report for this exact (epoch, target, sentinel) is already held. */
    bool HaveReport(uint32_t nEpoch, const uint256& target, const uint256& sentinel) const;

    /** Advance the current epoch and drop everything older than the window. */
    void SetCurrentEpoch(uint32_t nEpoch);

    size_t Size() const;

private:
    using Key = std::tuple<uint32_t, uint256, uint256>; // (epoch, target, sentinel)

    mutable Mutex m_mutex;
    std::map<Key, CPoSeServiceReport> m_reports GUARDED_BY(m_mutex);
    uint32_t m_currentEpoch GUARDED_BY(m_mutex){0};
    const uint32_t m_keepEpochs;

    // Lowest epoch still inside the retained window for m_currentEpoch.
    uint32_t OldestKeptEpoch() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
};

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_STORE_H
