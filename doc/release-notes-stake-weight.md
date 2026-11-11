Staking figures and messages
----------------------------

- The network stake weight (`netstakeweight` in `getstakinginfo`) assumed a
  valid coinstake time every 6 seconds; with this chain's time mask one comes
  every 4 seconds, so the figure was about 1.5 times too high. It is now about
  two thirds of what it was, and so is the expected time to a reward
  (`expectedtime`), which is computed from it.
- The automatic stake target size is computed from the same figure. Between its
  lower and upper limits it is now about two thirds of what it was; at a limit
  it can stay the same, and a size set with `-staketarget` is not affected.
  With a smaller target a wallet can split a staked amount into more
  outputs, and stops combining small outputs at a smaller size, than before.
  Which blocks and coinstakes are valid, and their format, are unchanged.
- When staking cannot be switched on for a wallet (it is locked, holds no
  private keys, its coinstake descriptors cannot be added, or it was loaded a
  moment ago and the staking list does not have it yet), the reason is now
  written to the log without `-debug=pos`.
