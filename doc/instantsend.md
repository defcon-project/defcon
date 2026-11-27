# InstantSend in DeFCoN Core

InstantSend locks a transaction using a masternode quorum. Block inclusion and ChainLocks are separate events. DeFCoN uses deterministic InstantSend locks (`ISDLOCK`). On mainnet, the v23 quorum profile for InstantSend and ChainLocks is `llmq_defcon` (Q60). The node selects the profile from its network parameters and current chain tip.

## Check a transaction

For a wallet transaction, use `defcon-cli -rpcwallet=<name> gettransaction <txid>`. For a transaction available to the node, use `defcon-cli getrawtransaction <txid> true`. In those responses, `instantlock_internal` reports a direct InstantSend lock. The broader `instantlock` flag can also be true when a ChainLock covers the transaction’s block.

For an unconfirmed transaction, `defcon-cli getmempoolentry <txid>` returns `instantlock` as a string: `"true"`, `"false"` or `"unknown"`.

## Receive notifications

Configure `-zmqpubhashtxlock=<address>` for transaction IDs or `-zmqpubrawtxlock=<address>` for serialized transactions. Subscribers use the topics `hashtxlock` and `rawtxlock`. See the [ZeroMQ guide](zmq.md) for the multipart message format and connection settings.

A wallet-enabled node can also run a local command when one of its wallet transactions receives a lock. Set `-instantsendnotify=<cmd>`; the node substitutes the transaction ID for `%s`.

## Block inclusion

When InstantSend conflict filtering is active, an unlocked transaction normally waits up to the two-minute age threshold before it becomes eligible for a block. A lock removes this particular wait. Fees, other block policy and block production still determine inclusion.
