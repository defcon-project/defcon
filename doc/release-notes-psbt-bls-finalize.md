Wallet
------

- `walletprocesspsbt` with `finalize=false` now finalizes an input it signs
  with a BLS key. A BLS signature does not fit the partial signature record of
  a PSBT, so the PSBT the wallet returned was refused by the next step,
  `decodepsbt`, `combinepsbt` and `finalizepsbt` of the same node included.
  Inputs signed with ECDSA keys stay partial signatures as before.
