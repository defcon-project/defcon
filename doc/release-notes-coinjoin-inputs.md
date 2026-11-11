CoinJoin
--------

- A masternode now looks up the inputs of a mixing request's collateral through
  the same mempool-aware coin view as the mixing inputs themselves, and holds the
  chain lock while it reads them. A collateral that spends an output which does
  not exist is refused like any other invalid collateral.
- The amounts of a mixing request's collateral are checked against the money
  range before they are added up, so a collateral with an amount out of range is
  refused early.
