CoinJoin
--------

- A masternode now looks up the inputs of a mixing request's collateral through
  the same mempool-aware coin view as the mixing inputs themselves, and holds the
  chain lock while it reads them. A collateral that spends an output which does
  not exist is refused like any other invalid collateral.
