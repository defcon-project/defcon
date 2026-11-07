Consensus, dormant until the v23.1 height: a block with no payable masternode
------------------------------------------------------------------------------

If masternodes are registered but none of them can be paid, because every one is
banned or suspended, a staked block has no masternode payout. The honest producer
already leaves the coinbase empty in that case, but until now no rule checked it, so
a modified producer could have paid itself there. From the v23.1 height the
coinbase of such a block may hold nothing but its empty first output: no payout,
and no extra data.

Two things follow. The height cannot be scheduled before superblocks are retired,
nor together with the platform share of masternode rewards (mn_rr), since either
could put an output of its own into that coinbase. Every release network already
retires superblocks earlier and schedules no platform share, so neither changes
anything there; a regtest that schedules the height names both.

Nothing changes before the height. It stays unset on mainnet and testnet until the
release; a devnet can schedule it with `-v231activationheight=<n>`, and a regtest
with `-testactivationheight=v231@<n>`. The condition itself is rare: it needs every
masternode on the network to be banned at once.
