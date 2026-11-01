Logging
-------

- Masternodes no longer log "quorum initialization failed" for quorum types that do not form on the
  network at that height. The line stays for a type that does form but has too few members.

RPC changes
-----------

- `quorum dkginfo`: `next_dkg` now counts only quorum types that form at that height, and
  `upcoming_dkgs` no longer lists a cycle at or above a type's formation end. On a chain above the
  Q60 formation the value of `next_dkg` does not change, because `llmq_defcon` shares the 24-block
  grid of the types it no longer follows.
