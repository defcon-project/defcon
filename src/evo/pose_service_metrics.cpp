// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_metrics.h>
#include <logging.h>

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace dsl {
namespace {
std::atomic<bool> enabled{false};
std::mutex metrics_mutex;
PerfSnapshot metrics;
constexpr std::array<const char*, static_cast<size_t>(PerfMetric::COUNT)> names{
    "wire_report", "budget_refused", "decode_refused", "base_refused",
    "accepted", "duplicate", "refused", "store_wait", "store_hold",
    "assignment_hit", "assignment_miss", "assignment", "verify_request",
    "verify_invalid", "bls_verify", "bls_success", "cache_hit", "cache_miss",
    "commitment_build", "signing_build", "miner_build", "rpc_build",
    "emit_reports", "inverse_assignment", "report_sign", "relay_serialize", "relay_push", "tip_queue",
    "activation_refused"
};
}

bool PerfEnabled() { return enabled.load(std::memory_order_relaxed); }
void SetPerfEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

void RecordPerf(uint32_t epoch, const uint256& base, PerfMetric metric, uint64_t nanos)
{
    if (!PerfEnabled()) return;
    const auto index = static_cast<size_t>(metric);
    if (index >= names.size()) return;
    std::lock_guard<std::mutex> lock(metrics_mutex);
    PerfEpoch* free_slot{nullptr};
    for (auto& slot : metrics.epochs) {
        if (slot.used && slot.epoch == epoch && slot.base == base) {
            ++slot.values[index].count;
            slot.values[index].nanos += nanos;
            return;
        }
        if (!slot.used && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) {
        ++metrics.overflow;
        return;
    }
    free_slot->used = true;
    free_slot->epoch = epoch;
    free_slot->base = base;
    free_slot->values[index] = {1, nanos};
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
    }
    if (snapshot.overflow != 0) LogPrintf("DSL perf -- measurement overflow=%u\n", snapshot.overflow);
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
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - m_start).count();
    RecordPerf(m_epoch, m_base, m_metric, static_cast<uint64_t>(elapsed));
}
} // namespace dsl
