The v23.1 activation height over RPC
------------------------------------

- `getblockchaininfo` lists the v23.1 upgrade under `softforks` as `v231`, in
  the form of a buried deployment: `height` is the activation height, and
  `active` tells whether the next block is at or above it. The entry is there
  only where the height is scheduled: on mainnet and testnet from the release
  that sets it, on a devnet started with `-v231activationheight=<n>`, and on a
  regtest with `-testactivationheight=v231@<n>`. A node whose
  `help getblockchaininfo` describes `v231` and whose reply does not list it
  carries no activation height. Earlier builds never list it, scheduled or
  not.
