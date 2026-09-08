// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_faults.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <tinyformat.h>
#include <util/underlying.h>

#include <algorithm>

namespace dsl {

const char* const FAULT_INJECTION_ARG = "-enablefaultinjection";

namespace {
constexpr std::string_view KIND_NAMES[] = {
    "response-drop",
    "report-drop",
    "response-delay",
    "report-delay",
    "commitment-skip",
};
static_assert(std::size(KIND_NAMES) == ToUnderlying(FaultKind::_COUNT), "every FaultKind needs a name");

bool IsDelayKind(FaultKind kind)
{
    return kind == FaultKind::RESPONSE_DELAY || kind == FaultKind::REPORT_DELAY;
}
} // namespace

std::string_view FaultKindName(FaultKind kind)
{
    if (ToUnderlying(kind) >= ToUnderlying(FaultKind::_COUNT)) return "unknown";
    return KIND_NAMES[ToUnderlying(kind)];
}

std::optional<FaultKind> FaultKindFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(KIND_NAMES); ++i) {
        if (KIND_NAMES[i] == name) return static_cast<FaultKind>(i);
    }
    return std::nullopt;
}

std::vector<FaultKind> AllFaultKinds()
{
    std::vector<FaultKind> kinds;
    for (size_t i = 0; i < std::size(KIND_NAMES); ++i) kinds.push_back(static_cast<FaultKind>(i));
    return kinds;
}

size_t CFaultInjector::ExpireLocked(int currentHeight)
{
    const auto before = m_faults.size();
    m_faults.erase(std::remove_if(m_faults.begin(), m_faults.end(),
                                  [currentHeight](const Fault& f) { return !f.IsActiveAt(currentHeight); }),
                   m_faults.end());
    return before - m_faults.size();
}

std::optional<Fault> CFaultInjector::Set(FaultKind kind, int currentHeight, int expiryHeight, uint32_t param,
                                         std::string scenarioId)
{
    if (!m_enabled) return std::nullopt;
    if (ToUnderlying(kind) >= ToUnderlying(FaultKind::_COUNT)) return std::nullopt;
    if (expiryHeight <= currentHeight) return std::nullopt;
    if (scenarioId.empty()) return std::nullopt;
    if (IsDelayKind(kind) && param == 0) return std::nullopt;
    LOCK(m_mutex);
    ExpireLocked(currentHeight);
    Fault fault;
    fault.id = m_nextId++;
    fault.kind = kind;
    fault.setAtHeight = currentHeight;
    fault.expiryHeight = expiryHeight;
    fault.param = param;
    fault.scenarioId = std::move(scenarioId);
    m_faults.push_back(fault);
    return fault;
}

std::vector<Fault> CFaultInjector::List(int currentHeight) const
{
    std::vector<Fault> active;
    if (!m_enabled) return active;
    LOCK(m_mutex);
    for (const auto& fault : m_faults) {
        if (fault.IsActiveAt(currentHeight)) active.push_back(fault);
    }
    return active;
}

std::optional<Fault> CFaultInjector::Active(FaultKind kind, int currentHeight) const
{
    if (!m_enabled) return std::nullopt;
    LOCK(m_mutex);
    for (const auto& fault : m_faults) {
        if (fault.kind == kind && fault.IsActiveAt(currentHeight)) return fault;
    }
    return std::nullopt;
}

std::optional<Fault> CFaultInjector::Apply(FaultKind kind, int currentHeight)
{
    if (!m_enabled) return std::nullopt;
    LOCK(m_mutex);
    for (auto& fault : m_faults) {
        if (fault.kind == kind && fault.IsActiveAt(currentHeight)) {
            ++fault.hits;
            return fault;
        }
    }
    return std::nullopt;
}

bool CFaultInjector::Clear(uint64_t id)
{
    LOCK(m_mutex);
    const auto it = std::find_if(m_faults.begin(), m_faults.end(), [id](const Fault& f) { return f.id == id; });
    if (it == m_faults.end()) return false;
    m_faults.erase(it);
    return true;
}

size_t CFaultInjector::Clear()
{
    LOCK(m_mutex);
    const auto n = m_faults.size();
    m_faults.clear();
    return n;
}

size_t CFaultInjector::Expire(int currentHeight)
{
    LOCK(m_mutex);
    return ExpireLocked(currentHeight);
}

bool FaultInjectionAllowedOn(const CChainParams& params)
{
    const std::string& id = params.NetworkIDString();
    return id == CBaseChainParams::DEVNET || id == CBaseChainParams::REGTEST;
}

std::optional<std::string> FaultInjectionRefusal(const CChainParams& params, bool requested)
{
    if (!requested) return std::nullopt;
    if (FaultInjectionAllowedOn(params)) return std::nullopt;
    return strprintf("%s is not available on the %s chain: DSL fault injection runs on devnet and regtest only",
                     FAULT_INJECTION_ARG, params.NetworkIDString());
}

} // namespace dsl
