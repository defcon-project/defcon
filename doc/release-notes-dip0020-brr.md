Removed: the DIP0020 and BRR activation switches
------------------------------------------------

The activation switches (deployments) of two Dash upgrades, DIP0020 and the
block reward reallocation (BRR), are removed. On mainnet, testnet and devnet
their activation heights were set to a value that is never reached; only
regtest, the local test network, had them switched on, from its first block.
The script opcodes DIP0020 introduced do not depend on the switch and are not
affected. Block validation, rewards and stored data do not change.

Visible only on regtest: `getblockchaininfo` no longer lists the `dip0020` and
`realloc` softforks (mainnet, testnet and devnet did not list them), and
`-testactivationheight=brr@<height>` is no longer accepted; a node started
with it stops with "Invalid name".
