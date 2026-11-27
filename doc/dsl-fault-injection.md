# DeFCoN Sentinel Layer test coverage

The DeFCoN Sentinel Layer records masternode service observations. Devnet and regtest include an opt-in fault simulator used to check how observations and commitments recover. Mainnet and public testnet reject the startup option for the simulator. On devnet and regtest, start the node with `-enablefaultinjection`, then arm, list and clear faults with the `faultinject` RPC, which accepts only cookie authentication.

## Test guarantees

- The simulator keeps its state in memory. A restart clears the state.
- A simulated condition has an expiry height. Normal processing resumes at that height.
- Test telemetry distinguishes a local observation from the commitment recorded in the chain.
- Tests compare observations across participants and check recovery after a simulated interruption ends.

The [unit tests](../src/test/dsl_fault_injection_tests.cpp) cover network gating, expiry and state management. The functional tests [feature_dsl_fault_injection.py](../test/functional/feature_dsl_fault_injection.py), [feature_dsl_faults.py](../test/functional/feature_dsl_faults.py) and [feature_dsl_scenarios.py](../test/functional/feature_dsl_scenarios.py) check the lifecycle, startup behaviour and the simulated faults on regtest. See the [Sentinel processing design note](sentinelfix.md) for how the node schedules Sentinel work.
