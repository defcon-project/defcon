Wallet unlock modes
-------------------

- Changing the wallet passphrase (`walletpassphrasechange`, or in the GUI) now
  leaves the wallet unlocked the way it was: locked, unlocked for mixing and
  staking only, or fully unlocked. Before, a wallet unlocked for mixing and
  staking only came out of the change fully unlocked from the call and locked
  from the GUI, or, after a wrong old passphrase, locked, and staking stopped.
- `getwalletinfo` shows the mode in a new field, `unlock_mode`: `locked`,
  `mixing_staking_only` or `full` (encrypted wallets only).
- Unchanged: a timed full unlock (`walletpassphrase` without `mixingonly`)
  ends with the wallet locked, also when it was unlocked for mixing and staking
  only before. To keep staking, unlock it for mixing and staking again after a
  full unlock ends, or use `walletlock` and `walletpassphrase ... true`.
