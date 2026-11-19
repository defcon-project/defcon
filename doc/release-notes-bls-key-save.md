BLS keys that could not be saved, and wallets without private keys
------------------------------------------------------------------

- A new BLS key that could not be written to the wallet file is no longer one
  of the wallet's keys. Before, `bls generate`, `importblsprivkey` or a payment
  that needed a BLS change address reported the failure, but the wallet went on
  listing the key's address (`listblsaddresses`), and a legacy wallet went on
  counting payments to it, until its next start, when the key was gone. A BLS
  key is not derived from the wallet's seed.
- A wallet created without private keys (`disable_private_keys`) takes no new
  BLS secret key. `bls generate` fails in it, and a legacy one whose payment to
  a BLS address leaves change asks for a change address instead of making a BLS
  key for it. A BLS key that an earlier version saved in such a wallet stays in
  its file: this version does not remove it.
