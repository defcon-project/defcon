Headers on a deep branch
------------------------

- A node takes headers that arrive without their blocks only on a branch that
  leaves its chain at most 100 blocks below its tip (a limit new in this
  release; v23.0.0 had none). A node that stayed on a chain the rest of the
  network left more than 100 blocks ago (for example one upgraded only after an
  activation, or cut off for a long time) therefore does not move to the
  network's chain on its own.
- Such a node now says so: a line in the log, at most once an hour, and a
  warning in `getnetworkinfo`, `getblockchaininfo`, `getmininginfo` and the GUI
  while its outbound peers keep offering such a chain. The warning clears a day
  after the last time they did.
- The way back is a trusted, up-to-date node added with `addnode` (or given the
  `noban` permission, for example `-whitelist=noban@<address>`): such a peer is
  not held to the 100-block limit, so the node can take its chain and move to
  it, if that chain is valid and does not conflict with a ChainLock this node
  has accepted. A conflicting ChainLock needs separate recovery; adding the peer
  is not enough then. The trust is real: such a peer can also add a longer
  branch of headers without blocks than other peers can.
