# The v23 activation on a copy of mainnet

This document records one rehearsal of the v23 activation on real mainnet
data, run on 2026-09-21 and 2026-09-22 against the release commit
`a06830fd42`. The regtest rehearsals (`feature_v23_bundle_heights.py`, #258;
`feature_v23_joint_activation_llmq.py`, #262) run on short, synthetic chains.
They cannot show what the activation does to a chain with mainnet's history,
its evo database, its masternode list and real coins of every age. This run
does, for the proof-of-stake side; the quorum side stays with the regtest
tests, because no masternode ran here.

## What was run

- **Data.** A copy of a mainnet data directory, brought to the live tip with
  the v22.1.4 release binary: height 139554, block `7f6415c9…c26531`.
- **Binary.** The release commit with one line changed:
  `V23_MAINNET_ACTIVATION_HEIGHT` from 144888 to **139704**, so that the
  activation arrives 150 blocks after the copy's tip instead of about 5300.
  Nothing else differs. The test height keeps the release height's place on
  the 72-block grid (139704 and 144888 are both 24 mod 72), and it puts
  H − 120 = 139584 thirty blocks past the tip, so that the protocol floor and
  the opening of Q60 formation happen on new blocks, not in the copied
  history. That binary existed only for this run and was not distributed.
- **Network.** Four nodes, each on its own copy, connected only to one another
  (`-connect`, `-dnsseed=0`, `-fixedseeds=0`), inside a rootless network
  namespace that holds a loopback interface and nothing else. Before any node
  started, a connection to a mainnet seed and one to a public internet address
  were attempted from inside the namespace, and both failed with "Network is
  unreachable". No block, transaction or connection of this run could reach
  mainnet.
- **Stake.** One node staked, with a copy of a real mainnet wallet lent for the
  run by its owner. The wallet had many stakeable outputs, some of them older
  than the 60-day stake-age limit that v23 lifts. No transaction was made on
  mainnet, and every copy of the wallet was deleted after the run.
- **Length.** From the copy's tip through H to H + 50 = 139754: 200 blocks in
  7.6 hours of real time, with a mean interval of 137.6 s.

## Measured

| | Observed |
|---|---|
| Stake weight at H − 2 and H − 1 | On tip 139702, the node reported its outputs older than 60 days as excluded (`getstakinginfo`, `excluded.too_old`). On tip 139703, where the weight is computed for block 139704 = H, that exclusion was gone. The weight had risen by exactly its amount, plus one staking reward that matured between the two readings. |
| The chain across H | 200 consecutive blocks, and the four nodes agreed on every tip. Before H, no staked output was older than 22 days. After H, block 139715 was staked by an output 204 days old, beyond the 60-day limit that applied below H. |
| Block value | Every block created exactly 10 500 DFCN: 10 000 to a masternode in the coinbase and 500 to the staker. |
| A fee-paying transaction after H | Sent at tip 139706 and mined in 139707, 152 s later: the two-minute wait for an InstantSend lock that cannot come here, then the next block. That block also created exactly 10 500 DFCN, so the fee was paid to no one. |
| At the end, 139754 | All four nodes reported the same tip hash, the same `gettxoutsetinfo` hash and the same `protx diff 1 139754`. |
| `-reindex -assumevalid=0` | One node rebuilt from its block files to the same tip hash in 210 s. Restarted with `-checklevel=4 -checkblocks=2000`, it found no inconsistency. |
| v22.1.4 on a data directory v23 has written | It refuses to start: "Error upgrading Evo database". |
| v22.1.4 with `-reindex`, on another copy | It rebuilds the shared history to 139593 and rejects 139594 = H − 110 with `bad-qc-commitment-type`. That block is the first of the Q60 mining window, and it carries the Q60 (null) commitment that a v22.1.4 node does not accept. On mainnet this is block 144778. |
| The protocol floor at H − 120 | Every node kept its connections, since all four ran the new protocol version. |

The test binary's `defcond` had sha256
`413a748101e110677e9c1a17c7d196fe0d0e0876b3de334890891f6c147cacf1`. The
v22.1.4 `defcond` was the released one,
`edc297dc601dd9431b09761125e63e92cfce37eaf28e0d132a441d10b533c13c`.
The run recorded here is the third attempt. Faults in the harness stopped the
first two before anything was measured: the isolation check read the host's
interface list instead of the namespace's, and a boolean reached the CLI as
`True`.

## What it does not show

- **Quorums signing.** No masternode ran, so no Q60 quorum formed and nothing
  was ChainLocked or InstantSend-locked; the blocks carried null commitments.
  That half is rehearsed on regtest (#262).
- **The Sentinel layer.** It starts at H + 576, beyond the end of this run.
- **Refusal by the new rules.** Every observation above is of honest blocks
  being produced and accepted. No invalid block was offered to the new rules;
  the one refusal measured is the old binary's.
- **The release height itself.** The binary carried a different H. That 144888
  is compiled into the release is shown by the unit tests, and by the release
  binaries' own start-up report.
- **A network.** One staker and four nodes ran on one machine, so the times
  above are not network times.
- **Mixed versions.** No v22.1.4 node ran beside the upgraded ones. The
  downgrade was measured on copies of one data directory.

## How to repeat it

1. Bring a copy of a mainnet data directory to the tip with the current
   release. Work on the copy, never on the original, and remove `wallets/` and
   `backups/` from it.
2. Build the release commit with `V23_MAINNET_ACTIVATION_HEIGHT` set to a
   height H that is 24 mod 72, with H − 120 a few dozen blocks above the copy's
   tip. Change nothing else, and record the diff and the binary's sha256.
3. Create a namespace with nothing but loopback
   (`unshare --user --map-root-user --net`, then `ip link set lo up`). From
   inside it, confirm that an outbound connection fails before starting
   anything. Read the interface list from `/proc/net/dev`, not from
   `/sys/class/net`, which still shows the host's interfaces. Run every node
   and every RPC call from inside the namespace.
4. Start four nodes on separate copies, connected only to one another. A
   mainnet staker needs three peers and a finished masternode sync. With
   `-maxtipage` raised, a copy a few hours old is not in initial block
   download.
5. Stake with a wallet that has at least a few dozen outputs inside the stake
   value range, all deeper than 26 blocks. Each won block locks its input for
   26 blocks, and nothing else extends this chain.
6. Read `getstakinginfo` on tips H − 2 and H − 1, and carry the chain to
   H + 50. Compare the nodes and reindex one. Then start v22.1.4 on two further
   copies, once normally and once with `-reindex`.
