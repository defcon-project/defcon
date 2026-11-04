RPC changes
-----------

- `getblockchaininfo` has a new `stake_double_signs` object. `count` is how many times since the
  node started it has seen a staker sign twice: two different blocks at one height that spend the
  same kernel and are both signed by that kernel's owner (the key the kernel's output pays), most
  often one wallet staking on two nodes at once. Blocks that name somebody else's kernel under a
  key of their own, which anyone can write, are not kept and never count. The latest one is
  named by `last_height`, `last_kernel` and `last_blocks`, and logged once. This is observation
  only: nothing is refused or penalised. Limits: a second block is seen only if the node accepts
  it, which in practice means one that arrives before the next block does; the record is kept in
  memory for the last 100 blocks below the tip and for at most 10,000 height and kernel pairs, and
  is lost on restart; a kernel whose output the node cannot read when it first sees a block on it
  (already spent, for example after a restart) is not tracked; and only kernels paying a public key
  or a public key hash, which are the ones the wallet stakes, have an owner key to compare.

- `getstakinginfo` has a new `stake_combine_rest_allowance` per wallet: what is left of the
  wallet's resting-value budget for optional consolidation (`stake_combine_rest_budget_bps`). When
  it is 0, a win combines only outputs under the minimum stakeable amount.

Logging
-------

- With `-debug=pos`, each win's coinstake plan line now says how many outputs consolidation left
  out and why: `budget` (over what may still be spent or set resting), `size` (no room for more
  inputs), `split` (taking it would not reduce the wallet's outputs) and `signature` (no key, or
  signing failed), next to `room`, the inputs the block still had space for. With
  `-stakecombine=0` the line says `combine=off`. Staking itself does not change.
