Network: staked blocks whose parent is missing
---------------------------------------------

- Once a node has finished its initial sync, it asks its peers for a proof-of-stake block, and takes one from them,
  only when it has the chain that block builds on; blocks from peers whose parent never arrives are no longer stored.
  A node that falls behind after its initial sync (for example after its computer slept) therefore downloads the
  blocks it missed one after another. A node in its initial sync downloads in parallel as before, and blocks given
  with `submitblock`, `-loadblock` or `-reindex`, or fetched with `getblockfrompeer`, are taken as before.
