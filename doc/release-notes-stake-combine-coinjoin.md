Wallet changes
--------------

- Staking no longer combines CoinJoin coins. With `-stakecombine` (on by default) a winning stake
  can also spend up to 19 of the wallet's small outputs, smallest first, within the block's size
  and the wallet's budget limits, and with the default `-stakecombinescope=wallet` it takes them
  from any address of the wallet. A wallet that mixed and staked could therefore spend some of its
  mixed coins and mixing collaterals with each win and pay them to its staking address, which
  linked them on the chain. Staking and `combineoutputs` now leave out every output whose amount
  is a CoinJoin denomination (10 000.1, 1 000.01, 100.001, 10.0001, 1.00001, 0.100001, 0.0100001
  or 0.00100001 DFCN) or a CoinJoin collateral amount (0.0001 to 0.0004 DFCN), in both scopes.
  On testnet and regtest, where the staking floor is zero, a stake won by a coin of a CoinJoin
  collateral amount takes no other output with it. Only the wallet changes; blocks and network
  rules stay the same.
- `combineoutputs` no longer reports a batch with an output of exactly a CoinJoin denomination
  amount as stakeable, since staking skips such a coin: with `staking_only` or `output_size` a
  batch otherwise within the staking limits fails with "Batch cannot be laid out within staking
  amount limits", otherwise it reports `stakeable_amounts` false. A preview now sizes each batch for the kind of address the send pays
  to (P2SH for a `sh(pkh(...))` receiving descriptor), still without taking a key.
- Known limitation: the rule goes by amount, so an ordinary output of exactly one of these amounts
  is left out too; it can still be spent normally. Coins combined before this change stay linked,
  and with the default scope staking still links the wallet's other addresses.
- Wallets that mix and still run an older version can turn combining off with `-stakecombine=0`, or
  keep it within the winning key with `-stakecombinescope=key`.
