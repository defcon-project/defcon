Wallet changes
--------------

- Staking no longer combines CoinJoin coins. With `-stakecombine` (on by default) a winning stake
  can also spend up to 19 of the wallet's small outputs, smallest first, within the block's size
  and the wallet's budget limits, and with the default `-stakecombinescope=wallet` it takes them
  from any address of the wallet. A wallet that mixed and staked could therefore spend some of its
  mixed coins and mixing collaterals with each win and pay them to its staking address, which
  linked them on the chain. Staking and `combineoutputs` now leave out every output whose amount
  is a CoinJoin denomination (10.0001, 1.00001, 0.100001, 0.0100001 or 0.00100001 DFCN) or a
  CoinJoin collateral amount (0.0001 to 0.0004 DFCN), in both scopes. On test networks, where any
  amount can stake, a stake won by such a coin takes no other output with it. Only the wallet
  changes; blocks and network rules stay the same.
- Known limitation: the rule goes by amount, so an ordinary output of exactly one of these amounts
  is left out too; it can still be spent normally. Coins combined before this change stay linked,
  and with the default scope staking still links the wallet's other addresses.
- Wallets that mix and still run an older version can turn combining off with `-stakecombine=0`, or
  keep it within the winning key with `-stakecombinescope=key`.
