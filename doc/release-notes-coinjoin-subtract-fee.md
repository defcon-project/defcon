CoinJoin payments with the fee subtracted from the amount
---------------------------------------------------------

- A payment from the mixed (CoinJoin) balance has no change: what its coins
  hold beyond the amount goes to the fee. With "Subtract fee from amount"
  switched on, that rest was taken twice, from the recipient's amount and from
  the coins, while the wallet showed it once, so the payment paid a higher fee
  than the wallet showed. Now the recipient's amount is reduced only by the fee
  the transaction needs, the rest of the coins is paid once, and the fee shown
  is the fee paid. Payments without fee subtraction, and payments that are not
  made from the mixed balance alone, are built as before.
