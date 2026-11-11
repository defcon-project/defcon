Block import
------------

- A damaged copy of a proof-of-stake block read on `-reindex` or `-loadblock`
  (a bad or overlong block signature, or a damaged coinstake) no longer marks
  the block as invalid, so the node takes the good copy from its peers instead
  of stalling. `submitblock` now answers `bad-txnmrklroot` for a
  proof-of-stake block whose coinstake was damaged so that the block reads as
  proof of work.
