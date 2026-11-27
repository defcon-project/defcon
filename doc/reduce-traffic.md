# Reduce node bandwidth use

DeFCoN Core normally targets up to eight full-relay outbound peers and two block-relay-only peers. Manual peers, temporary service connections and connection probes can add connections beyond those targets.

## Limit historical block uploads

Set `-maxuploadtarget=<MiB per day>` to reduce historical block serving as usage approaches the target. This setting defaults to unlimited. The target is not a hard bandwidth cap. Historical blocks are more than seven days older than the best-known header. Peers with the `download` permission can continue requesting them.

## Limit regular peer connections

For a regular full node, `-listen=0` disables listening for inbound P2P connections, and `-maxconnections=<n>` limits regular peer connections. Retain enough independent peers to sync reliably. A masternode requires listening and a connection limit of at least 125.

## Restrict transaction relay

Use `-blocksonly` to restrict transaction relay from peers. RPC transaction submission remains available. The wallet defaults to `-walletbroadcast=0` in this mode unless you set it explicitly. A node receiving few unconfirmed transactions will have less data for fee estimation. Broadcasting a wallet’s own transactions from a node that otherwise relays very few can reveal their origin.
