RPC changes
-----------

- `protx listdiff` now reports changes to the four Sentinel layer fields of a masternode state. An
  `updatedMNs` entry includes `missedServiceEpochs`, `lastServiceEpoch`, `rewardSuspended` and
  `dslBanHeight` when they changed, under the same names and types as the full masternode state.
  Previously a change to only these fields appeared as an empty object. Existing keys are
  unchanged.
