Unauthenticated REST Interface
==============================

The REST API can be enabled with the `-rest` option.

The interface runs on the same port as the JSON-RPC interface, by default port 8193 for mainnet and port 19998 for testnet.

REST Interface consistency guarantees
-------------------------------------

The [same guarantees as for the RPC Interface](/doc/JSON-RPC-interface.md#rpc-consistency-guarantees)
apply.

Limitations
-----------

Keep client concurrency bounded. Each open HTTP connection uses a file descriptor.

Supported API
-------------

#### Transactions
`GET /rest/tx/<TX-HASH>.<bin|hex|json>`

Given a transaction hash: returns a transaction in binary, hex-encoded binary, or JSON formats.
Responds with 404 if the transaction doesn't exist.

The endpoint searches the mempool and, when available, the transaction index. DeFCoN Core enables the index by default. If you disabled it with `txindex=0`, this endpoint can only find mempool transactions.

#### Blocks
- `GET /rest/block/<BLOCK-HASH>.<bin|hex|json>`
- `GET /rest/block/notxdetails/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns a block, in binary, hex-encoded binary or JSON formats.
Responds with 404 if the block doesn't exist.

The HTTP request and response are both handled entirely in-memory.

With the /notxdetails/ option JSON response will only contain the transaction hash instead of the complete transaction details. The option only affects the JSON response.

#### Blockheaders
`GET /rest/headers/<COUNT>/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns <COUNT> amount of blockheaders in upward direction.
Returns empty if the block doesn't exist or it isn't in the active chain.

#### Blockfilter Headers
`GET /rest/blockfilterheaders/<FILTERTYPE>/<COUNT>/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns <COUNT> amount of blockfilter headers in upward
direction for the filter type <FILTERTYPE>.
Returns empty if the block doesn't exist or it isn't in the active chain.

#### Blockfilters
`GET /rest/blockfilter/<FILTERTYPE>/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns the block filter of the given block of type
<FILTERTYPE>.
Responds with 404 if the block doesn't exist.

#### Blockhash by height
`GET /rest/blockhashbyheight/<HEIGHT>.<bin|hex|json>`

Given a height: returns hash of block in best-block-chain at height provided.
Responds with 404 if block not found.

#### Chaininfos
`GET /rest/chaininfo.json`

Returns various state info regarding block chain processing.
Only supports JSON as output format.
Use `defcon-cli help getblockchaininfo` for fields and types. For example, `blocks` is the active tip height and `bestblockhash` is its hash.

#### Query UTXO set
- `GET /rest/getutxos/<TXID>-<N>/<TXID>-<N>/.../<TXID>-<N>.<bin|hex|json>`
- `GET /rest/getutxos/checkmempool/<TXID>-<N>/<TXID>-<N>/.../<TXID>-<N>.<bin|hex|json>`

The getutxos endpoint allows querying the UTXO set, given a set of outpoints.
With the `/checkmempool/` option, the mempool is also taken into account.
See [BIP64](https://github.com/bitcoin/bips/blob/master/bip-0064.mediawiki) for
input and output serialization (relevant for `bin` and `hex` output formats).

On mainnet, set `TXID` and `VOUT` to an outpoint from your node, then query it locally.

```sh
curl --fail "http://127.0.0.1:8193/rest/getutxos/checkmempool/${TXID}-${VOUT}.json"
```

#### Memory pool
`GET /rest/mempool/info.json`

Returns various information about the TX mempool.
Only supports JSON as output format.
Refer to the `getmempoolinfo` RPC for documentation of the fields.

`GET /rest/mempool/contents.json`

Returns transactions in the TX mempool.
Only supports JSON as output format.
Refer to the `getrawmempool` RPC help for details.

Risks
-------------
The REST interface has no authentication. Bind it to a trusted interface and restrict which local users, applications and remote clients can reach it.
