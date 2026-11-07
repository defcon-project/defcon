Quorum rotation removed
-----------------------

The never-activated Dash quorum rotation (DIP0024) is gone. No public DeFCoN network registered a rotating
quorum type, so blocks, quorums, ChainLocks and InstantSend on those networks are unchanged.

- The `quorum rotationinfo` RPC is removed. On the public networks it only ever returned an error.
- The P2P messages `getqrinfo` and `qrinfo` are ignored: a request gets no answer and no penalty, and an
  unsolicited `qrinfo` is no longer penalized. Their names and v2 short IDs stay reserved.
- `quorum listextended` now reports `quorumIndex` (always 0) for every quorum, as `quorum info` does. Before,
  it reported it only for rotating quorums, which no public network had.
- Regtest: the `dip0024` softfork and the `-testactivationheight=dip0024@<height>` option are gone, the
  rotating `llmq_test_dip0024` quorum type no longer exists, and InstantSend is signed by
  `llmq_test_instantsend`, which is now always enabled, with one quorum per 24-block DKG cycle.
- Regtest: an existing regtest chain made by an earlier version can stop working with this version. Measured
  on such a chain made with the default settings: block 442, the first quorum mining window after DIP3, lacks
  the `llmq_test_instantsend` commitment this version requires and is rejected, so `-reindex` stops at block
  441. From block 444 on, the old blocks also carry commitments of the removed rotating quorum type, which this
  version cannot disconnect: a reorganization that has to disconnect such a block fails, and the node refuses
  to start ("Corrupted block database detected") when such a block is among the recent blocks it checks at
  startup. To start a new chain, stop the node and rename the `regtest` directory rather than deleting it:
  without `-walletdir`, the regtest wallets are inside it. Mainnet, testnet and devnet did not register a
  rotating quorum type or `llmq_test_instantsend` before this change either, and the mainnet chain holds no
  rotating commitment, so their data is not affected.
