Logging and RPC help
--------------------

- A block refused as `time-too-new` is logged with the limit the rule applied,
  the node's adjusted time plus this chain's 3 minutes, rather than plus
  Bitcoin's two hours. The help of `importmulti`, `importdescriptors` and
  `pruneblockchain` gives the same window in seconds where it said two hours.
  The rule itself does not change.
