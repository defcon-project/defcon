Locked wallets and BLS addresses
--------------------------------

- An encrypted legacy (non-descriptor) wallet now knows the payments to its own
  BLS addresses while it is locked or unlocked for mixing and staking only.
  Before, such a payment was not recorded when it arrived in that state and
  stayed missing until a rescan (`rescanblockchain`), and recorded ones dropped
  out of the balance while the wallet was locked. Spending them needs the full
  unlock, as before.
- `listblsaddresses` lists the BLS addresses of a locked wallet as well, legacy
  or descriptor; it returned none for one before.
- The wallet file format is unchanged. A legacy wallet that missed such
  payments under an earlier version needs one rescan to find them.
