# Reduce node memory use

Use `defcond -help` for the options supported by your build. Lower cache and peer limits can slow initial sync and processing.

| Option | Default | Effect |
| --- | --- | --- |
| `-dbcache=<MiB>` | 300 MiB | Maximum database cache allocation, with a 4 MiB minimum. Unused mempool memory can also be used for caching. |
| `-maxmempool=<MB>` | 300 MB | Limits transaction pool memory. A smaller pool evicts transactions sooner. The minimum depends on `-limitdescendantsize`. |
| `-maxconnections=<n>` | 125 | Limits regular peer connections. Manual `-addnode` connections have a separate limit of eight; temporary service connections are excluded. A masternode requires at least 125. |
| `-rpcthreads=<n>` | 4 | Sets the number of HTTP RPC service threads. |

The database cache uses binary MiB. The mempool limit uses decimal MB. Compare actual memory usage and sync speed after changing either setting.

The `-blocksonly` option restricts peer transaction relay. Transactions submitted through RPC can still enter the mempool, so it is not a memory limit. See [reducing traffic](reduce-traffic.md) for its effect on wallets and peers.

On Linux systems with glibc, `MALLOC_ARENA_MAX` can cap the allocator’s arena count. Measure resident memory and throughput on the target system before keeping a change.
