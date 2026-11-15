Wallet transactions that could not be saved
-------------------------------------------

- When the wallet cannot write a transaction to its file (a full or failing
  disk), it logs an error and no longer moves on the block it has recorded as
  processed, so the next start rescans from there and finds the transaction
  again. Before, the transaction could be missing from the wallet after a
  restart.
- A rescan now fails when it cannot save a transaction it found, and goes on
  failing until the transaction is saved: `rescanblockchain`, `upgradetohd`
  and the rescan at startup report it, as do `importmulti`, `importprivkey`,
  `importaddress`, `importpubkey` and `importwallet`. `importelectrumwallet`
  does not check the result of its rescan.
- `importprunedfunds` returns an error when the transaction could not be saved,
  instead of reporting success.
