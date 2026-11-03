RPC changes
-----------

- `getblockchaininfo` has a new `chainlock` object. `enabled` tells whether ChainLocks are switched
  on. While the node enforces a ChainLock, `height`, `blockhash` and `time` name the locked block,
  and `age_blocks` counts the blocks of the active chain above the newest block a ChainLock covers:
  0 while the tip is locked. These are the blocks a reorg can still replace. If the ChainLock
  quorum stops signing, the chain goes on without ChainLocks and `age_blocks` keeps growing, which
  a monitor can alert on. Existing keys are unchanged.
