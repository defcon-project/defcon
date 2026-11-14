Network: headers and blocks that arrive without their data
----------------------------------------------------------

- A proof-of-stake header costs nothing to make, and every header a node stores stays in its block
  index for good. A node now stores a header that comes without its block only up to 24 blocks above
  its tip (more when its tip is behind, so a node catching up still follows the real chain), not when
  it branches off more than 100 blocks below the tip (a peer added with `addnode` or given the `noban`
  permission is exempt; see "Headers on a deep branch"), and not when it conflicts with a ChainLock.
  Each peer can also open at most 32 new branches in 24 hours with headers whose blocks it does not
  send, or with blocks the node did not ask for; inbound peers of one network group share that number
  (see "New branches from inbound peers"). Whatever is over these limits is dropped without
  penalising the peer, so nodes are not disconnected for sending it.
- A node left on its own branch for more than 100 blocks does not move back on its own: it warns, and
  a trusted peer brings it back (see "Headers on a deep branch").
- Headers announced above the tip whose blocks never arrive no longer keep the masternode sync -- and
  with it staking and the masternode services -- waiting after a restart: the sync's blockchain stage
  now also ends once the node's own chain tip is less than 30 minutes old.
