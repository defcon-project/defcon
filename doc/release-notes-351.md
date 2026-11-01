Devnet: rehearsing the v23.1 upgrade
------------------------------------

The new devnet-only option `-v231activationheight=<n>` schedules the v23.1 upgrade on a
devnet without rebuilding the binary. From that height the masternode payment falls from
10000 to 5000 DFCN per block, `llmq_400_60` and `llmq_400_85` stop forming, and no
superblock path skips the coinstake ceiling.

The height must be a multiple of 576, above the devnet's Q60 ChainLocks switchover and not
below its proof-of-stake coinbase bound, so the lowest it can take is 8064. Every node must
run with the same value before that height, because a node without it forks off there. The
option has no effect on mainnet, testnet or regtest, where every v23.1 height stays unset
until the release.
