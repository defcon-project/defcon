Change of payments to BLS addresses
-----------------------------------

- A descriptor wallet that pays a BLS address now sends its change to an
  ordinary address of its own. Before, the change went to a new BLS key, which
  a descriptor wallet stores but does not count as its own: the change was
  missing from its balance and could not be spent from it. Change that earlier
  versions sent this way stays stored in the wallet file, but this version does
  not show or spend it either.
- A legacy wallet still sends the change of a payment to a BLS address to a new
  BLS key. That key is not derived from the wallet's seed, so back up the wallet
  file after such a payment (`dumpwallet` does not include BLS keys).
- Only a payment to a BLS public key counts as one. Before, any output script of
  50 bytes or more, such as a data carrier, also sent the change to a new BLS
  key.
- A descriptor wallet stores BLS keys (`bls generate`) but does not count
  payments to their addresses as its own; `listblsaddresses` and the help of
  `bls generate` now say so. To receive at a BLS address, use a legacy wallet.
