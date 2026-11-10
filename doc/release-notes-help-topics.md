RPC changes
-----------

- `help` lists the commands by topic. The `Evo` and `DeFCoN` groups are gone: their commands are now
  under `CoinJoin`, `Governance`, `Keys and signatures`, `Masternodes`, `Network security` and
  `Sentinel`; `combineoutputs` moved to `Staking`, and `setcoinjoinrounds` and `setcoinjoinamount` to
  `CoinJoin`. Developer commands (`faultinject`, `quorum dkgsimerror`) and deprecated ones
  (`getunconfirmedbalance`, and `getassetunlockstatuses` and `quorum platformsign`, left over from Dash
  Platform) are no longer listed by `help`, by the subcommand lists of `help quorum` and
  `help masternode`, or by the completion of the Qt console. They still run, and `help <command>`
  still describes them. `help masternode` now lists `connect`, and no longer the hidden `current` and
  `winner`. Command names, arguments and replies do not change.

GUI changes
-----------

- The console's `help-console` page begins with a short way in to this network's commands, and it can
  be translated.
