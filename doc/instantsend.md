## InstantSend Technical Information

InstantSend has been integrated into the Core Daemon in two ways:
* "push" notifications (ZMQ and `-instantsendnotify` cmd-line/config option);
* RPC commands.

#### ZMQ

When a "Transaction Lock" occurs the hash of the related transaction is broadcasted through ZMQ using both the `zmqpubrawtxlock` and `zmqpubhashtxlock` channels.

* `zmqpubrawtxlock`: publishes the raw transaction when locked via InstantSend
* `zmqpubhashtxlock`: publishes the transaction hash when locked via InstantSend

This mechanism has been integrated into Bitcore-Node-Dash which allows for notification to be broadcast through Insight API in one of two ways:
* WebSocket: [https://github.com/dashpay/insight-api-dash#web-socket-api](https://github.com/dashpay/insight-api-dash#web-socket-api)
* API: [https://github.com/dashpay/insight-api-dash#instantsend-transactions](https://github.com/dashpay/insight-api-dash#instantsend-transactions)

#### Command line option

When a wallet InstantSend transaction is successfully locked a shell command provided in this option is executed (`%s` in `<cmd>` is replaced by TxID):

```
-instantsendnotify=<cmd>
```

#### RPC

Details pertaining to an observed "Transaction Lock" can also be retrieved through RPC. There is a boolean field named `instantlock` which indicates whether a given transaction is locked via InstantSend. This field is present in the output of some wallet RPC commands e.g. `listsinceblock`, `gettransaction` etc. as well as in the output of some mempool RPC commands e.g. `getmempoolentry` and a couple of others like `getrawmempool` (for `verbose=true` only). For blockchain based RPC commands `instantlock` will also say `true` if this transaction was locked via LLMQ based ChainLocks (for backwards compatibility reasons).

#### Which quorum signs

An InstantSend lock is signed by the LLMQ profile that
`llmq::GetInstantSendLLMQType()` resolves for the current chain tip: the
network's `llmqTypeDIP0024InstantSend` below `nInstantSendV2ActivationHeight`,
and `llmqTypeDIP0024InstantSendV2` at and above it. Signer and verifier both
read the tip, so every node switches profile at the same block. The locks are
deterministic (`ISDLOCK`) and do not need DIP0024 quorum rotation: DeFCoN never
activated DIP0024, and its InstantSend profiles do not rotate.

On the devnet the switchover moves InstantSend onto the Q60 profile
(`llmq_defcon`), the same quorum that signs ChainLocks. Mainnet keeps its
legacy configuration until the v23 activation sets the V2 pair together with
the rest of that bundle; until then no InstantSend quorum exists on mainnet
and no lock is ever produced there.

A transaction that is not locked may still be mined once it has waited
`WAIT_FOR_ISLOCK_TIMEOUT` (two minutes) in the mempool, and a ChainLock signer
applies the same wait before signing a block that contains it. A locked
transaction is mined at once.
