// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_metrics.h>
#include <chain.h>
#include <consensus/params.h>
#include <logging.h>
#include <util/time.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace dsl {
namespace {
std::atomic<bool> enabled{false};
std::atomic<int64_t> test_process_delay_ms{0};
std::mutex metrics_mutex;
PerfSnapshot metrics;
thread_local std::optional<std::chrono::steady_clock::time_point> tip_event_queued;
constexpr std::array<const char*, static_cast<size_t>(PerfMetric::COUNT)> names{
    "wire_report", "budget_refused", "decode_refused", "base_refused",
    "accepted", "duplicate", "refused", "store_wait", "store_hold",
    "assignment_hit", "assignment_miss", "assignment", "verify_request",
    "verify_invalid", "bls_verify", "bls_success", "cache_hit", "cache_miss",
    "commitment_build", "signing_build", "miner_build", "rpc_build",
    "emit_reports", "inverse_assignment", "report_sign", "relay_serialize", "relay_push", "tip_queue",
    "activation_refused", "decode_report", "duplicate_undecoded",
    "peerman_tip_run", "dkg_tip_start", "dsl_tick_run", "block_msg_wait",
    "decode_response", "duplicate_response_undecoded"
};
constexpr std::array<PerfMetric, PERF_LATENCY_METRICS> latency_metrics{
    PerfMetric::TIP_QUEUE, PerfMetric::PEERMAN_TIP_RUN, PerfMetric::DKG_TIP_START, PerfMetric::DSL_TICK_RUN,
    PerfMetric::BLOCK_MSG_WAIT};

// The slot of (epoch, base), taken if it is new; nullptr, counted as overflow, when every slot holds
// another key.
PerfEpoch* Slot(uint32_t epoch, const uint256& base)
{
    PerfEpoch* free_slot{nullptr};
    for (auto& slot : metrics.epochs) {
        if (slot.used && slot.epoch == epoch && slot.base == base) return &slot;
        if (!slot.used && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) {
        ++metrics.overflow;
        return nullptr;
    }
    free_slot->used = true;
    free_slot->epoch = epoch;
    free_slot->base = base;
    return free_slot;
}
} // namespace

std::optional<size_t> LatencyIndex(PerfMetric metric)
{
    for (size_t i = 0; i < latency_metrics.size(); ++i) {
        if (latency_metrics[i] == metric) return i;
    }
    return std::nullopt;
}

unsigned PerfBucket(uint64_t nanos)
{
    unsigned log2{0};
    while (log2 < 63 && (nanos >> (log2 + 1)) != 0) ++log2;
    if (log2 < PERF_BUCKET_LOW) return PERF_BUCKET_LOW;
    if (log2 > PERF_BUCKET_HIGH) return PERF_BUCKET_HIGH;
    return log2;
}

uint32_t PerfPosition(int height)
{
    return height < 0 ? 0 : static_cast<uint32_t>(height) % PERF_POSITIONS;
}

bool PerfEnabled() { return enabled.load(std::memory_order_relaxed); }
void SetPerfEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

void RecordPerf(uint32_t epoch, const uint256& base, PerfMetric metric, uint64_t nanos)
{
    if (!PerfEnabled()) return;
    const auto index = static_cast<size_t>(metric);
    if (index >= names.size()) return;
    std::lock_guard<std::mutex> lock(metrics_mutex);
    PerfEpoch* slot = Slot(epoch, base);
    if (slot == nullptr) return;
    ++slot->values[index].count;
    slot->values[index].nanos += nanos;
}

void RecordLatency(uint32_t epoch, const uint256& base, PerfMetric metric, uint32_t position, uint64_t nanos)
{
    if (!PerfEnabled()) return;
    const auto index = static_cast<size_t>(metric);
    if (index >= names.size()) return;
    const auto latency = LatencyIndex(metric);
    std::lock_guard<std::mutex> lock(metrics_mutex);
    PerfEpoch* slot = Slot(epoch, base);
    if (slot == nullptr) return;
    ++slot->values[index].count;
    slot->values[index].nanos += nanos;
    if (!latency || position >= PERF_POSITIONS) return;
    if (slot->latency.empty()) slot->latency.resize(PERF_LATENCY_METRICS * PERF_POSITIONS);
    PerfHistogram& histogram = slot->latency[*latency * PERF_POSITIONS + position];
    ++histogram.buckets[PerfBucket(nanos) - PERF_BUCKET_LOW];
    if (nanos >> (PERF_BUCKET_HIGH + 1) != 0) ++histogram.overflow;
}

PerfSnapshot TakePerfSnapshot()
{
    std::lock_guard<std::mutex> lock(metrics_mutex);
    return std::exchange(metrics, PerfSnapshot{});
}

void LogPerfSummary()
{
    if (!PerfEnabled()) return;
    const auto snapshot = TakePerfSnapshot();
    for (const auto& slot : snapshot.epochs) {
        if (!slot.used) continue;
        std::string fields;
        for (size_t i = 0; i < names.size(); ++i) {
            if (slot.values[i].count == 0) continue;
            fields += strprintf(" %s=%u/%uns", names[i], slot.values[i].count, slot.values[i].nanos);
        }
        LogPrintf("DSL perf -- epoch=%u base=%s%s\n", slot.epoch, slot.base.ToString(), fields);
        if (slot.latency.empty()) continue;
        for (size_t m = 0; m < latency_metrics.size(); ++m) {
            for (uint32_t pos = 0; pos < PERF_POSITIONS; ++pos) {
                const PerfHistogram& histogram = slot.latency[m * PERF_POSITIONS + pos];
                uint64_t samples{0};
                std::string buckets;
                for (size_t b = 0; b < PERF_BUCKETS; ++b) {
                    if (histogram.buckets[b] == 0) continue;
                    samples += histogram.buckets[b];
                    buckets += strprintf("%s%u:%u", buckets.empty() ? "" : ",", b + PERF_BUCKET_LOW, histogram.buckets[b]);
                }
                if (samples == 0) continue;
                LogPrintf("DSL perf hist -- epoch=%u base=%s metric=%s pos=%u n=%u over=%u buckets=%s\n", slot.epoch,
                          slot.base.ToString(), names[static_cast<size_t>(latency_metrics[m])], pos, samples,
                          histogram.overflow, buckets);
            }
        }
    }
    if (snapshot.overflow != 0) LogPrintf("DSL perf -- measurement overflow=%u\n", snapshot.overflow);
}

uint64_t NanosSince(std::chrono::steady_clock::time_point start)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

std::optional<PerfKey> TipPerfKey(const CBlockIndex* pindex, const Consensus::Params& consensus)
{
    if (pindex == nullptr || consensus.nDSLEpochInterval <= 0 || pindex->nHeight < consensus.nDSLActivationHeight) {
        return std::nullopt;
    }
    const auto epoch = static_cast<uint32_t>(pindex->nHeight / consensus.nDSLEpochInterval);
    const CBlockIndex* base = pindex->GetAncestor(static_cast<int>(epoch) * consensus.nDSLEpochInterval);
    if (base == nullptr) return std::nullopt;
    return PerfKey{epoch, base->GetBlockHash(), PerfPosition(pindex->nHeight)};
}

LatencyTimer::LatencyTimer(const std::optional<PerfKey>& key, PerfMetric metric)
    : m_key(PerfEnabled() ? key : std::nullopt), m_metric(metric)
{
    if (m_key) m_start = std::chrono::steady_clock::now();
}
LatencyTimer::~LatencyTimer()
{
    if (m_key) RecordLatency(m_key->epoch, m_key->base, m_metric, m_key->position, NanosSince(m_start));
}

TipEventScope::TipEventScope(std::chrono::steady_clock::time_point queued) : m_previous(tip_event_queued)
{
    tip_event_queued = queued;
}
TipEventScope::~TipEventScope() { tip_event_queued = m_previous; }
std::optional<std::chrono::steady_clock::time_point> TipEventQueued() { return tip_event_queued; }

void SetTestProcessDelay(std::chrono::milliseconds delay)
{
    test_process_delay_ms.store(std::max<int64_t>(0, delay.count()), std::memory_order_relaxed);
}
void TestProcessDelay()
{
    const int64_t delay = test_process_delay_ms.load(std::memory_order_relaxed);
    if (delay > 0) UninterruptibleSleep(std::chrono::milliseconds{delay});
}

PerfTimer::PerfTimer(uint32_t epoch, const uint256& base, PerfMetric metric)
    : m_enabled(PerfEnabled()), m_epoch(epoch), m_base(base), m_metric(metric)
{
    if (m_enabled) m_start = std::chrono::steady_clock::now();
}
PerfTimer::~PerfTimer() { Stop(); }
void PerfTimer::Stop()
{
    if (!m_enabled) return;
    m_enabled = false;
    RecordPerf(m_epoch, m_base, m_metric, NanosSince(m_start));
}
} // namespace dsl
