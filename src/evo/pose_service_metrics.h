// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_METRICS_H
#define BITCOIN_EVO_POSE_SERVICE_METRICS_H

#include <uint256.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class CBlockIndex;
namespace Consensus {
struct Params;
} // namespace Consensus

namespace dsl {

// Diagnostics only: no counter, timer or overflow affects report admission.
enum class PerfMetric : size_t {
    WIRE_REPORT, BUDGET_REFUSED, DECODE_REFUSED, BASE_REFUSED,
    ACCEPTED, DUPLICATE, REFUSED,
    STORE_WAIT, STORE_HOLD, ASSIGNMENT_HIT, ASSIGNMENT_MISS, ASSIGNMENT,
    VERIFY_REQUEST, VERIFY_INVALID, BLS_VERIFY, BLS_SUCCESS, CACHE_HIT, CACHE_MISS,
    COMMITMENT_BUILD, SIGNING_BUILD, MINER_BUILD, RPC_BUILD,
    EMIT_REPORTS, INVERSE_ASSIGNMENT, REPORT_SIGN, RELAY_SERIALIZE, RELAY_PUSH, TIP_QUEUE,
    ACTIVATION_REFUSED,
    // Reading a POSEREPORT off the wire (count and time, whether or not it read), and the copies of a
    // held report dropped from their bytes without being read; the latter are also counted as DUPLICATE.
    DECODE_REPORT, DUPLICATE_UNDECODED,
    // Where a block's processing waits (sentinelfix.md 4.1), each also kept per DKG-cycle position
    // (RecordLatency): the run of PeerManager's UpdatedBlockTip, from an UpdatedBlockTip event being
    // queued to the DKG notification starting on it, the Sentinel tick's run, and a block, compact block,
    // block transactions or headers message waiting from its arrival until its processing starts.
    PEERMAN_TIP_RUN, DKG_TIP_START, DSL_TICK_RUN, BLOCK_MSG_WAIT,
    // Reading a POSERESPONSE off the wire (count and time, whether or not it read), and the copies of a
    // liveness announcement dropped from their bytes without being read (none yet: that short circuit is
    // the next change's, SF1-A).
    DECODE_RESPONSE, DUPLICATE_RESPONSE_UNDECODED,
    COUNT
};

struct PerfValue {
    uint64_t count{0};
    uint64_t nanos{0};
};

//! The latency metrics, in the order of their histograms: TIP_QUEUE, PEERMAN_TIP_RUN, DKG_TIP_START,
//! DSL_TICK_RUN and BLOCK_MSG_WAIT.
static constexpr size_t PERF_LATENCY_METRICS{5};
//! The position of a block in the 24-block DKG cycle of llmq_defcon (and of regtest's llmq_test, which has
//! the same timing): height mod 24.
static constexpr uint32_t PERF_POSITIONS{24};
//! Log2 buckets of nanoseconds: bucket k holds [2^k, 2^(k+1)) ns for k = 10..49; anything below 2^10 ns
//! goes to bucket 10, and anything from 2^50 ns up to bucket 49 and the histogram's overflow count.
static constexpr unsigned PERF_BUCKET_LOW{10};
static constexpr unsigned PERF_BUCKET_HIGH{49};
static constexpr size_t PERF_BUCKETS{PERF_BUCKET_HIGH - PERF_BUCKET_LOW + 1};

struct PerfHistogram {
    std::array<uint32_t, PERF_BUCKETS> buckets{};
    uint32_t overflow{0};
};

//! The index of a latency metric among the PERF_LATENCY_METRICS, or nullopt for any other metric.
std::optional<size_t> LatencyIndex(PerfMetric metric);
//! The log2 bucket (PERF_BUCKET_LOW..PERF_BUCKET_HIGH) a duration in nanoseconds falls in.
unsigned PerfBucket(uint64_t nanos);
//! A block height's position in the DKG cycle (height mod PERF_POSITIONS); 0 for a negative height.
uint32_t PerfPosition(int height);

struct PerfEpoch {
    bool used{false};
    uint32_t epoch{0};
    uint256 base;
    std::array<PerfValue, static_cast<size_t>(PerfMetric::COUNT)> values{};
    //! Per latency metric and position (index LatencyIndex * PERF_POSITIONS + position); allocated on the
    //! slot's first latency sample, so that a snapshot handed around stays small.
    std::vector<PerfHistogram> latency;
};
static constexpr size_t PERF_EPOCH_SLOTS{16};
struct PerfSnapshot {
    std::array<PerfEpoch, PERF_EPOCH_SLOTS> epochs{};
    uint64_t overflow{0};
};

bool PerfEnabled();
void SetPerfEnabled(bool enabled);
void RecordPerf(uint32_t epoch, const uint256& base, PerfMetric metric, uint64_t nanos = 0);
//! RecordPerf, and for a latency metric also the sample in the histogram of its DKG-cycle position.
void RecordLatency(uint32_t epoch, const uint256& base, PerfMetric metric, uint32_t position, uint64_t nanos);
PerfSnapshot TakePerfSnapshot(); // reads and resets the bounded measurement window
//! Called once per epoch transition; formatting outside locks. Besides the one summary line per epoch it
//! writes one "DSL perf hist" line per latency metric and non-empty position, which
//! contrib/devtools/dsl_perf_pct.py turns into percentiles.
void LogPerfSummary();

//! Monotonic nanoseconds since `start`, never negative; mocktime does not move it.
uint64_t NanosSince(std::chrono::steady_clock::time_point start);

//! Where a block-tip metric is filed: the block's DSL epoch, that epoch's base block, and the block's
//! DKG-cycle position.
struct PerfKey {
    uint32_t epoch{0};
    uint256 base;
    uint32_t position{0};
};
//! The key of a block, or nullopt before DSL activation (the condition TIP_QUEUE has always had).
std::optional<PerfKey> TipPerfKey(const CBlockIndex* pindex, const Consensus::Params& consensus);

//! Times its scope into a latency metric when it has a key; does nothing without one.
class LatencyTimer {
    std::optional<PerfKey> m_key;
    PerfMetric m_metric;
    std::chrono::steady_clock::time_point m_start{};
public:
    LatencyTimer(const std::optional<PerfKey>& key, PerfMetric metric);
    ~LatencyTimer();
    LatencyTimer(const LatencyTimer&) = delete;
    LatencyTimer& operator=(const LatencyTimer&) = delete;
};

//! While the validation queue runs the callbacks of one UpdatedBlockTip event, when that event was queued
//! (for DKG_TIP_START). The queue runs one event at a time on its own thread, so this is kept per thread.
class TipEventScope {
    std::optional<std::chrono::steady_clock::time_point> m_previous;
public:
    explicit TipEventScope(std::chrono::steady_clock::time_point queued);
    ~TipEventScope();
    TipEventScope(const TipEventScope&) = delete;
    TipEventScope& operator=(const TipEventScope&) = delete;
};
std::optional<std::chrono::steady_clock::time_point> TipEventQueued();

//! -dsltestprocessdelay (regtest and devnet only; startup is refused elsewhere, TestProcessDelayRefusal): a
//! slow node for tests. TestProcessDelay sleeps for it where the work it stands for is done -- after reading
//! a POSEREPORT or POSERESPONSE, after the tick checks a held liveness announcement, after signing an own
//! report -- and returns at once when it is zero, as it is outside tests.
void SetTestProcessDelay(std::chrono::milliseconds delay);
void TestProcessDelay();

class PerfTimer {
    bool m_enabled;
    uint32_t m_epoch;
    uint256 m_base;
    PerfMetric m_metric;
    std::chrono::steady_clock::time_point m_start{};
public:
    PerfTimer(uint32_t epoch, const uint256& base, PerfMetric metric);
    ~PerfTimer();
    PerfTimer(const PerfTimer&) = delete;
    PerfTimer& operator=(const PerfTimer&) = delete;
    void Stop();
};

} // namespace dsl
#endif // BITCOIN_EVO_POSE_SERVICE_METRICS_H
