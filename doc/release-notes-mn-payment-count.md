Consensus, dormant until the v23.1 height: each masternode payment in its own output
------------------------------------------------------------------------------------

A masternode whose operator takes a share of the payment is paid in two coinbase
outputs, the owner's and the operator's. When the operator's share is exactly half
and is paid to the owner's own address, the two outputs are identical, and until
now one output could stand for both: a modified block producer could pay the
masternode half and keep the other half. From the v23.1 height every expected
payment needs a coinbase output of its own.

The honest producer has always added both outputs, so nothing changes for
masternode owners, operators or stakers. Nothing changes before the height either:
blocks below it are checked as before, and it stays unset on mainnet and testnet
until the release.
