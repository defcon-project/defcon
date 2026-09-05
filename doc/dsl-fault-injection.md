# DSL fault injection (test networks only)

This document is the plan for injecting faults into the DeFCon Sentinel Layer
(DSL) so its response, report, aggregation and commitment paths can be
exercised on regtest and devnet without touching production behaviour. The
first change ships the state model, the startup gate and the admin RPC; the
hooks that make the faults act follow, one path at a time, each with its own
tests. Nothing in this document describes a way to trigger a fault on mainnet,
because none exists.

## What must always hold

- **Mainnet cannot arm it, from anywhere.** `-enablefaultinjection=1` is
  refused at startup on every chain except devnet and regtest
  (`dsl::FaultInjectionRefusal`, a pure function of the chain parameters,
  covered on all four chains by `dsl_fault_injection_tests`). Without the flag
  the injector is never enabled, and the RPC that would arm a fault answers
  "disabled". There is no second switch.
- **Nothing is persisted.** The injector holds its faults in memory only: no
  serialisation methods, no database, no file. A restart is a complete
  recovery. No fault state can reach consensus data, the evodb or the
  chainstate, because nothing writes it anywhere.
- **Every fault expires by height.** A fault is active while
  `height < expiryHeight`; the expiry block itself is already clean. A fault
  that would expire at or below the current height is refused rather than
  stored dead. Height, not wall time: the chain is the clock every observer
  shares.
- **Every fault names its scenario.** The `scenarioId` is required and is
  carried in the RPC output and the log line, so an observation in telemetry
  or the audit trail can be attributed to an experiment.
- **The RPC is local by construction.** `faultinject` answers only a caller
  authenticated with the datadir cookie (`__cookie__`), never an rpcuser or
  rpcauth credential. An orchestrator reaches it through the node-local
  wrapper on the host, the same way it reaches service and network faults.
- **Normal mode is untouched.** With the injector disabled, every hook is a
  null check on a null pointer and the DSL path is byte-for-byte what it was.
  The consensus-string fingerprint used for rollouts must not change.

## The model

```
FaultKind: response-drop | report-drop | response-delay | report-delay | commitment-skip
Fault:     id, kind, setAtHeight, expiryHeight, param, scenarioId
```

`CFaultInjector` (`src/evo/pose_service_faults.h`):

| Call | Effect |
|---|---|
| `Set(kind, currentHeight, expiryHeight, param, scenarioId)` | arms a fault; refuses when disabled, dead on arrival, unattributed, or a delay of zero |
| `List(currentHeight)` | the faults still active, oldest first |
| `Active(kind, currentHeight)` | the oldest active fault of a kind -- what a hook asks |
| `Clear(id)` / `Clear()` | drop one, or all |
| `Expire(currentHeight)` | sweep the expired ones |

Ids are never reused within a process.

## The RPC

```
faultinject set <kind> <expiryHeight> <scenarioId> [param]
faultinject list
faultinject clear [id]
```

Errors: `-1` when the injector is disabled (the flag was not given, or the
chain refused it), `-8` for an unknown kind, an expiry not above the tip, an
empty scenario or a zero delay, and an invalid-request error for a caller
without cookie authentication.

## What each kind does, and where

| Kind | Hook | Effect while active |
|---|---|---|
| `response-drop` | the per-epoch announce in `ProcessDSLTick`, and the relay of this node's own announcement | the node stays silent: its sentinels report it MISSED |
| `report-drop` | the cutoff emit in `ProcessDSLTick` | this node's reports never enter the pool |
| `response-delay` | announce | the announcement leaves `param` blocks late -- after the cutoff it is indistinguishable from a drop |
| `report-delay` | emit | the reports leave `param` blocks late -- after the aggregation offset they miss the commitment |
| `commitment-skip` | the signing step, and the miner's commitment inclusion | as a quorum member the node withholds its share; as a miner it mines no commitment that epoch |

Each hook is one `Apply()` lookup guarded by a null check, placed beside the
existing behaviour rather than inside it, so the normal path is not
restructured. A hook consults the fault *before* it marks the epoch's action
done, so a fault that expires or is cleared mid-epoch lets the very next
block perform the action: recovery is a block away, not an epoch. A delay is
measured from the action's normal offset in the epoch; a delay that reaches
past the epoch is a drop for that epoch. The targeted re-announcement a
`POSECHALLENGE` asks for obeys the response faults too, so a held
announcement cannot leak through a re-request. `faultinject list` reports
`hits` per fault: how many times it actually held an action back, which is
the shadow metric an experiment reads alongside the sentinels' verdicts. Relay of *other* nodes' messages is deliberately not a fault
kind: dropping relayed traffic partitions the pool in a way that depends on
topology, and the network-degradation faults already cover that from outside
the process.

## How it is proven

- Unit: the gate on main, test, devnet and regtest; the inert disabled
  injector; expiry semantics at the boundary; refusals; ids and clearing;
  kind names.
- Functional (`feature_dsl_fault_injection.py`): an armed and a plain node on
  regtest; set, expiry by mined blocks, refusals, clear, and that a restart
  forgets every fault; a restart without the flag refuses again.
- Functional (`feature_dsl_faults.py`): seven regtest masternodes probing each
  other. A `response-drop` on a running masternode makes its sentinels report
  it MISSED at the cutoff while the fault counts its hits; `clear` before the
  next epoch has it reported ONLINE again; a `report-drop` leaves that node's
  reports out of its peers' pools. The unfaulted epochs are the control.
- Fingerprint: the consensus-string fingerprint of the binary is unchanged by
  this change, which is checked at rollout time.

## Rollout

The devnet fleet does not carry `-enablefaultinjection`; only a lab node or an
explicitly approved pilot host does, for the span of an experiment, and the
experiment record names the flag, the scenario ids and the expiry heights.
