New branches from inbound peers
-------------------------------

- A node takes only a limited number of new branches a day from a peer when
  they come as headers without their blocks, or as blocks it did not ask for
  (32 that are never connected, a limit new in this release). For inbound peers
  this number is now kept for the peer's network group, not for the single
  connection, so reconnecting does not start it again.
- Inbound peers of one network group therefore share the number. Outbound
  peers, peers added with `addnode` and peers with the `noban` permission keep
  their own, so the node goes on following the chain through them.
