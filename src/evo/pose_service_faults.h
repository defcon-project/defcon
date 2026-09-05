// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_FAULTS_H
#define BITCOIN_EVO_POSE_SERVICE_FAULTS_H

#include <sync.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CChainParams;

namespace dsl {

/**
 * Fault injection for the Sentinel Layer (DSL), test networks only.
 *
 * This is the state model and the gate. It decides nothing about liveness by
 * itself: a fault is a request that the network layer, when it is wired to ask
 * (a later change), drops or delays a DSL message or skips a commitment for a
 * bounded number of blocks. What it must guarantee on its own, and what the
 * tests beside it pin:
 *
 *  - It is inert unless the node was started with -enablefaultinjection=1,
 *    and that argument is refused at startup on any chain that is not devnet
 *    or regtest. Mainnet cannot arm it from the configuration, and the RPC
 *    that would arm it answers "disabled" there because the injector was never
 *    enabled. The refusal is a pure function of the chain parameters
 *    (FaultInjectionRefusal), so it is testable without a node.
 *  - Every fault expires by height. Active means `height < expiryHeight`; the
 *    expiry block itself is already clean. A fault that would expire at or
 *    below the current height is refused rather than stored dead.
 *  - Nothing here is serialised. There is deliberately no SERIALIZE_METHODS, no
 *    database handle and no file: the state lives in this object and dies with
 *    the process, so a restart is a full recovery, and no fault can ever reach
 *    consensus data, the evodb or the chainstate.
 *  - Every fault names the scenario that asked for it, so telemetry and audit
 *    can attribute an observation to an experiment rather than to the network.
 *
 * The existing `quorum dkgsimerror` is the precedent this avoids repeating: it
 * is a process-global rate with no chain gate, no expiry and no owner, which a
 * forgotten call keeps applying into later rounds on whatever network the node
 * runs. Faults here are objects with ids, heights and a scenario, behind a
 * gate.
 */

enum class FaultKind : uint8_t {
    RESPONSE_DROP,   //!< do not send / relay this node's liveness announcement
    REPORT_DROP,     //!< do not send / relay this node's sentinel reports
    RESPONSE_DELAY,  //!< hold the announcement for `param` blocks
    REPORT_DELAY,    //!< hold the reports for `param` blocks
    COMMITMENT_SKIP, //!< as a signing-quorum member, do not sign / mine the commitment
    _COUNT,
};

/** The wire and RPC spelling of a kind: "response-drop", "report-drop", ... */
std::string_view FaultKindName(FaultKind kind);
std::optional<FaultKind> FaultKindFromName(std::string_view name);
/** Every kind, in enum order -- for help texts and exhaustive tests. */
std::vector<FaultKind> AllFaultKinds();

struct Fault {
    uint64_t id{0};
    FaultKind kind{FaultKind::RESPONSE_DROP};
    int setAtHeight{0};
    int expiryHeight{0};
    /** Kind-specific: the delay in blocks for the *_DELAY kinds, unused otherwise. */
    uint32_t param{0};
    /** Who asked. Required, never empty. */
    std::string scenarioId;

    bool IsActiveAt(int height) const { return height < expiryHeight; }
};

class CFaultInjector
{
public:
    explicit CFaultInjector(bool enabled) : m_enabled(enabled) {}

    /** Whether the node was started with the gate open. A disabled injector refuses Set and lists nothing. */
    bool Enabled() const { return m_enabled; }

    /**
     * Arm a fault. Refuses (nullopt) when disabled, when the expiry is not
     * strictly above the current height, when the scenario id is empty, or
     * when a delay kind has no delay. Expired faults are swept on the way.
     */
    std::optional<Fault> Set(FaultKind kind, int currentHeight, int expiryHeight, uint32_t param,
                             std::string scenarioId);

    /** The faults still active at `currentHeight`, oldest first. */
    std::vector<Fault> List(int currentHeight) const;

    /** The oldest active fault of this kind, if any. What the network layer will ask. */
    std::optional<Fault> Active(FaultKind kind, int currentHeight) const;

    /** Drop one fault by id; false if there was none. */
    bool Clear(uint64_t id);
    /** Drop every fault; returns how many. */
    size_t Clear();
    /** Drop the faults that have expired at `currentHeight`; returns how many. */
    size_t Expire(int currentHeight);

private:
    const bool m_enabled;
    mutable Mutex m_mutex;
    uint64_t m_nextId GUARDED_BY(m_mutex){1};
    std::vector<Fault> m_faults GUARDED_BY(m_mutex);

    size_t ExpireLocked(int currentHeight) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
};

/** The startup argument. Boolean; DEBUG_ONLY. */
extern const char* const FAULT_INJECTION_ARG;

/** True on the two chains that may run with faults: devnet and regtest. */
bool FaultInjectionAllowedOn(const CChainParams& params);

/**
 * The startup gate as a pure function: the error to refuse startup with when
 * fault injection was requested on a chain that does not allow it, otherwise
 * nullopt. Not requesting it is never an error anywhere.
 */
std::optional<std::string> FaultInjectionRefusal(const CChainParams& params, bool requested);

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_FAULTS_H
