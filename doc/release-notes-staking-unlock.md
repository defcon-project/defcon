Wallet changes
--------------

- A wallet unlocked for mixing only (`walletpassphrase <passphrase> <timeout> true`) can now also
  stake. Sending, message signing and exporting keys stay refused in that state, exactly as before,
  and the wallet is locked again after a restart. An encrypted staking wallet therefore no longer
  has to be fully unlocked to produce blocks. If CoinJoin mixing is enabled and running, this unlock
  also lets it go on mixing, as it always did.
- Signing a partially signed transaction now always needs a full unlock. Unlike the other sending
  RPCs, `send` did not check for one, so with a wallet unlocked for mixing only it could send; it
  now returns the transaction unsigned, as a PSBT, and does not send it. The PSBT and multisig
  dialogs of the desktop wallet signed in that state even when the full unlock they asked for was
  cancelled; they now report the inputs as unsigned.
- Locking the wallet at the very moment it was being unlocked for mixing only could leave it fully
  unlocked. Locking and unlocking now take turns, so the wallet ends up in the state one of them
  asked for. Likewise `listdescriptors true` now checks for a full unlock under the same wallet lock
  as the export itself, as `dumpprivkey` and `dumpwallet` already did.
- Known limitation, unchanged: some commands check for the full unlock only when they start --
  sending from the desktop wallet, `gobject prepare` and governance votes, `protx update_registrar`.
  If, while one of them is still running, the wallet is locked and unlocked for mixing only (which
  needs the passphrase), it can still complete. Started while the wallet is unlocked for mixing only,
  they are refused.
