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
    COUNT
};

struct PerfValue {
    uint64_t count{0};
    uint64_t nanos{0};
};
struct PerfEpoch {
    bool used{false};
    uint32_t epoch{0};
    uint256 base;
    std::array<PerfValue, static_cast<size_t>(PerfMetric::COUNT)> values{};
};
static constexpr size_t PERF_EPOCH_SLOTS{16};
struct PerfSnapshot {
    std::array<PerfEpoch, PERF_EPOCH_SLOTS> epochs{};
    uint64_t overflow{0};
};

bool PerfEnabled();
void SetPerfEnabled(bool enabled);
void RecordPerf(uint32_t epoch, const uint256& base, PerfMetric metric, uint64_t nanos = 0);
PerfSnapshot TakePerfSnapshot(); // reads and resets the bounded measurement window
void LogPerfSummary(); // called once per epoch transition; formatting outside locks

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
#endif
