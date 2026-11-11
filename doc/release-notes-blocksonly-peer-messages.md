Blocks-only mode
----------------

- A node started with `-blocksonly`, in a build with the wallet, now handles
  the CoinJoin queue's network messages and `getcoinjoininfo` correctly.
  `getcoinjoininfo` keeps its `queue_size` field there, with the value 0.
