# Export the active chain in block order

These tools use a DeFCoN node's RPC block hashes to copy matching blocks from local `blkNNNNN.dat` files in chain order. The source block files must contain the requested range from genesis. The data tool uses the [dash_hash module](https://github.com/dashpay/dash_hash) to compute X11 block hashes.

## Configure the export

Start with [example-linearize.cfg](example-linearize.cfg). Use a separate destination for the export because the data tool opens its output files for writing. Set:

| Setting | Value |
| --- | --- |
| `datadir` | Directory containing the running node's RPC cookie. As an alternative, set both `rpcuser` and `rpcpassword`. |
| `host`, `port` | RPC endpoint. The defaults are `127.0.0.1` and mainnet port `8193`. |
| `min_height`, `max_height` | Inclusive RPC hash range. `linearize-data.py` exports only from a hash list that contains the genesis block, so use `min_height=0` and a `max_height` the node has reached. |
| `input` | Directory containing the node's block files. |
| `hashlist` | Path to the hash list generated below. The default is `hashlist.txt`. |
| `output_file` or `output` | A single destination file or an existing output directory for numbered files. |

The sample contains example credentials, paths and a height. Replace those values for the local node.

## Run the export

With the configuration saved as `linearize.cfg`, run both commands from this directory:

```sh
python3 linearize-hashes.py linearize.cfg > hashlist.txt
python3 linearize-data.py linearize.cfg
```

`netmagic` and `genesis` default to DeFCoN mainnet. `rev_hash_bytes` reverses the byte order in the intermediate list when both tools use the same setting. For directory output, `max_out_sz` controls file rotation and `split_timestamp=1` also starts a new file at a month boundary. The [data script](linearize-data.py) defines the remaining settings, including the out-of-order block cache and output timestamps.
