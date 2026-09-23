# DeFCoN Core v23.0.0 — release notes


---

*Updated 2026-09-22: the height from which v22.1.4 can no longer follow the chain is 144 778, as
measured, not 144 888 as first stated; two further rehearsals added to "What has been measured".
The binaries, checksums and tag are unchanged.*

## What you must do, and by when

| | Block | Estimated time (UTC) |
|---|---|---|
| **Upgrade every node by** | **144 768** | ~2026-10-01 08:10 |
| Activation | **144 888** | ~2026-10-01 13:25 |
| Sentinel layer starts recording | **145 464** | ~2026-10-02 14:30 |

**Upgrade to v23.0.0 before block 144 768.** A node still running v22.1.4 at that height is
dropped by its upgraded peers and stops receiving the chain. It does not reach the activation
at all.

**Back up your data directory first**, with the node stopped.

The times are **estimates**, not promises. They are computed from the chain tip at publication at
the spacing mainnet has actually been running at — about 157 seconds against a 150-second target.
Ten days out that is worth several hours in either direction. **The block heights are exact; the
clock times are not.** Recompute them from the current tip if you are planning a maintenance
window near the deadline.

**Upgrade early, and do not coordinate a single moment.** Any time between now and block 144 768
is fine. If a large share of masternodes restart within the same few minutes, the quorum round
covering that window can fail.

---

## What v23 is

One number. `V23_MAINNET_ACTIVATION_HEIGHT` is **144 888** in this release
(`src/chainparams.cpp:197`). `V23_TESTNET_ACTIVATION_HEIGHT` is left unset (`:199`), so v23
schedules nothing on testnet.

At that one height, **nine** consensus changes turn on at once, in one block, on mainnet only:

| # | What | Field |
|---|---|---|
| 1 | ChainLocks move to the Q60 profile (`llmq_defcon`, 60 members, signing threshold 41) | `llmqTypeChainLocksV2`, `nChainLocksV2ActivationHeight` |
| 2 | InstantSend moves to the Q60 profile | `llmqTypeDIP0024InstantSendV2`, `nInstantSendV2ActivationHeight` |
| 3 | Proof-of-stake kernel v2: the stake-weighted comparison divides the kernel hash by the stake instead of multiplying the target, and the upper bound on a coin's age stops applying | `nPosKernelV2ActivationHeight` |
| 4 | The coinbase of a proof-of-stake block may carry only the payouts the rules expect | `nPosCoinbaseBoundActivationHeight` |
| 5 | The stake modifier is recomputed from the kernel the block staked | `nPosStakeModifierV2ActivationHeight` |
| 6 | A proof-of-stake block's time must be strictly after its predecessor's | `nPosBlockTimeBoundActivationHeight` |
| 7 | A proof-of-stake block's fees are destroyed by rule | `nPosFeeBurnActivationHeight` |
| 8 | The DKG bad-votes threshold on `llmq_400_60` rises from 30 to 300, the upstream figure for a 400-member quorum | `nDkgBadVotesV2ActivationHeight` |
| 9 | Superblocks are retired | `nSuperblocksRetiredHeight` |

They cannot be set individually. `ApplyV23ActivationBundle` (`chainparams.cpp:207-254`) writes all
nine from the one height, and `CheckV23ActivationBundle` refuses to start if they ever disagree.

Two further heights follow from the same number in code, rather than being set separately:

- the peer protocol floor moves **120 blocks earlier**, at **144 768**;
- the Sentinel layer begins observing **576 blocks later**, at **145 464** (`chainparams.cpp:253`).

**There is no network switch that can delay or cancel the activation.** It is compiled into the
release. Changing it would mean a new binary on every node before 144 768 — another reason not to
leave your upgrade to the last day.

---

## What changes the day you upgrade

The bundle itself sleeps until 144 888. These do not: they take effect the moment you run the new
binary.

| Change | Effect on a block that is valid today |
|---|---|
| A proof-of-stake block's nonce must be zero (#162/#188) | None. The block producer has always written a zero nonce at proof-of-stake heights (`node/miner.cpp:401`), so the rule asks of history exactly what the software already produced. |
| A header's proof of work is decided by height, not by the sender's nonce (#120) | None on history. It is a tightening at header level. |
| The early-collateral-spend rule is computed from the masternode list at the previous block instead of from a locally built cache (#119) | A real rule change, but it changes no block on the chain and requires nothing of you. |
| A block rejected for a bad stake kernel is now permanently marked invalid, so the node stops retrying it (#104) | None on the verdict. |
| The stake kernel is checked against the coins view the block is being validated with (#227) | None on a block's verdict. What changes is chain verification: `verifychain` at level 4 used to fail on any proof-of-stake chain, and a node started with `-addressindex`, `-spentindex` or `-timestampindex` is held to level 4 at startup — such a node refused an ordinary restart, and reindexing did not settle it. Those nodes restart normally now. |

And one behaviour change that is policy, not validity: **the wait for an InstantSend lock drops
from 600 to 120 seconds** (`llmq/chainlocks.h:51`). On mainnet no quorum can sign InstantSend
before 144 888, so a block producer keeps an unlocked transaction out of its template until it has
waited this out. From the day you upgrade that wait is two minutes instead of ten. A
high-fee transaction sitting in the mempool for that long is normal, not stuck, and raising the
fee does not change it.

---

## The ChainLock pause — read this if you run an exchange or a payment service

Between blocks **144 768 and 144 887** an upgraded node neither signs nor accepts a ChainLock for
any height inside that window. The blocks across it are the ones the network is mid-upgrade
through, and the Q60 quorums that will sign from 144 888 are only being formed.

- About five hours at the target spacing — a block count, not a guaranteed duration.
- **Blocks are still produced and transactions still confirm.** Only ChainLock finality is absent.
- The last lock from below the window stands and goes on verifying. Nothing that was already
  locked comes unlocked.
- `getbestchainlock` keeps returning that last lock for the whole window. **If your deposit policy
  credits on `chainlock: true`, or alerts when the ChainLock height stops advancing, it will stall
  for about five hours, and resume only once Q60 signs.** Plan for it, or raise your confirmation
  requirement for that window.

At 144 888 the pause ends. Locks resume under the Q60 profile as soon as a Q60 quorum has formed
and enough of its members are online to sign. Those quorums form in the 120 blocks before, and only
masternodes that have upgraded can take part in forming them: the more masternodes run v23.0.0 by
144 768, the sooner locking resumes.

---

## What the activation makes available

**InstantSend on mainnet.** Today mainnet has no InstantSend quorum that can sign, so every
transaction waits out the lock timeout before it can be mined. From 144 888 the Q60 quorum can
sign. On the devnet, where that profile has been signing since September, locks arrive in about
three seconds (median 2.7 s, p90 5 s, max 7 s over 40 transactions). The devnet is a much smaller
network and no lock has yet been signed on mainnet, so treat that as an indication, not a forecast.

**Q60 ChainLocks.** The new profile is 60 members with a signing threshold of 41. Any two groups
of 41 members share at least 22, and a member's node signs only one block per height. Two
ChainLocks for the same height would therefore need at least 22 members to sign against their own
software; a split of the network alone cannot produce them.

**The proof-of-stake rules**, items 3 to 7 above.

### If you stake, one of these you will see in your own wallet

Today a coin that has been unspent for more than 60 days is too old to stake: the node turns it
away, and `getstakinginfo` reports its value under `excluded.too_old`. That does not clear on its
own — until now the only way out was to spend the coin into a new output.

**From block 144 888 that upper limit is gone.** Such a coin is eligible again on the same terms as
any other, and that bucket empties. The lower limit stays: a coin still cannot stake in its first
hour, and the value range (10,000 to 12,500,000 DFCN per output) is unchanged.

Expect the network's total staking weight to rise at the activation, and your own share of blocks
to change accordingly.

**Your staking reward does not change**: it is and remains 500 DFCN per block. Transaction fees
have never been paid to the block producer on this chain — the wallet has always minted the
subsidy alone (`pos/stake.cpp:624`) — and from 144 888 that is a rule rather than a property of the
wallet. The rule is decided by height, so no block already on the chain is judged by it.

---

## If you run a masternode

**The deadline is not a convenience.** From block 144 768 your node is cut off from the upgraded
network, and from that same height the new quorums begin forming without it. A masternode that is
selected into rounds it cannot take part in accumulates PoSe penalties, and enough of them ban it.
A banned masternode stops being paid, and restarting does not clear it: after upgrading you must
send a ProUpServTx to revive it.

**Your BLS operator key is unchanged by the upgrade** and stays in your configuration file. Do not
regenerate it.

From block 145 464 your masternode announces itself once per hour, automatically. There is nothing
to configure and no extra process to run — see the Sentinel section.

---

## Upgrading, downgrading, and going back

**The deadline is 144 768, not 144 888.** From that height every upgraded node requires protocol
version **70242** or higher from each of its peers, whichever side opened the connection, and
there is no grace window: it drops peers it is already connected to as soon as the floor moves
(`net_processing.cpp:208-226`, `:3955-3966`, `:7275-7284`; `version.h:45`). The 120 is not a round
figure — it is the Q60 formation lead.

**A node still on the old version loses those peers and stops following the network's chain.**
Nothing the network mines after that point reaches it. Its own block count can still rise, because
the old software carries no such rule of its own: if other un-upgraded nodes are reachable and any
of them produces blocks, they carry on among themselves. **An un-upgraded node whose block count is
still climbing after 144 768 is on a chain of its own, not on the network's.**

**A node that upgrades late is not locked out.** The floor reads only the protocol version a peer
advertises, and a peer it drops is neither scored nor banned, so the node is admitted again as soon
as it runs v23. What it then has to do to catch up depends on what it did in the meantime: a node
that stood still has ordinary catching-up ahead of it; one that went on producing blocks of its own
comes back with a chain to reconcile. Neither case has been rehearsed end to end. Upgrade before
144 768 and the question does not arise.

**Going back is not a plain binary swap.** v22.1.4 does not understand the Evo database format v23
writes, so a downgrade needs `-reindex` or a backup taken before the upgrade.

**And once your node has followed the chain to 144 778 there is no way back to v22.1.4 at all** —
that binary cannot validate the chain from that block on, with or without a reindex. 144 778 is the
first block of the new quorum type's mining window, ten blocks after the deadline. The backup is
useful for a decision taken **before** 144 768, not after.

**If your node will be off across the activation, that is fine.** Upgrade the binary before you
start it again and it will sync forward. What you must not do is leave an un-upgraded node running
or syncing through the window: it can record a valid block as invalid and will keep refusing it
after you upgrade. If in doubt, stop the node, upgrade, and start it.

### Taking the backup

Stop the node and wait for it to exit fully, then copy the whole data directory — or at minimum
the `wallets` directory and your configuration file — while it is stopped. **A copy taken from a
running node may be unusable.**

---

## In the release, recording but not punishing: the Sentinel layer

The Sentinel liveness layer ships in v23 in two halves, and only the first of them is scheduled.

**On mainnet the observing half starts at block 145 464**, 576 blocks after the activation. That
height is derived from the activation height in code rather than set separately
(`chainparams.cpp:205`, `:253`). A node reports it itself: `dslstatus` shows `activationheight`
145 464 and `enforcementheight` 2147483647 — the unreachable value meaning the enforcing half has
no height in this release. The first service commitment can be mined at **145 488**: the epoch
containing the start of observation is never closed by a commitment, so the first one closes the
epoch after it.

From there the layer records which masternodes answered for their epoch, which did not, and which
the epoch could not judge either way, attested by the same Q60 quorums the activation brings into
being. **It records; it does not punish.** A missed hour is recorded and nothing else: no ban, no
lost payment, no penalty.

**The enforcing half is not in this release.** Its height stays unreachable, and a later release
will set it, if and only if the devnet's outage trials, a shadow measurement on mainnet after
145 464, and a replay of the enforcement arithmetic over recorded commitments all agree.

The commitment format is final in this release. A version-1 commitment carried only the set of
masternodes judged to have missed their duty, so a node the epoch's sentinels could not judge
healed as if it had been seen online; version 2 carries a second bitfield saying whom the epoch
actually judged, and an unjudged node's counters do not move. A release network has to use version
2 from its very first commitment, so the format had to be right before this release, not after.

Testnet is left unscheduled, so neither half has a height there.

---

## For block explorers, monitoring and integrations

- `masternode count` no longer returns a `detailed` breakdown. The block was the regular/Evo
  split, and the Evo masternode type — one no network here ever activated — was retired after
  v22.1.4. `total` and `enabled` are unchanged.
- The same retirement removed the rest of the Evo surface: the `evo` mode of `masternodelist` /
  `masternode list` and of `protx list`, and the four `protx *_evo` commands. A script that still
  calls one of them gets an error instead of a result.
- From 144 888, `superblocks_enabled` becomes false.
- From 144 888, a new quorum type name `llmq_defcon` appears wherever quorum types are enumerated,
  and in `getbestchainlock.llmqType`. **Anything parsing a fixed set of quorum type names must
  tolerate a new one.**
- From 144 888, `instantlock` starts being true on mainnet transactions.

---

## What you will see, and what it means

| What you see | What it is |
|---|---|
| `getbestchainlock` stops advancing between 144 768 and 144 887 | The ChainLock pause. Expected. Locks resume from 144 888, once a Q60 quorum has formed and signs. |
| Your peer count drops at 144 768, or at once if you upgrade after that block | The protocol floor. From that block your node disconnects peers that have not upgraded. |
| A high-fee transaction sits in the mempool for two minutes | The InstantSend wait. Raising the fee does not change it. |
| `dslstatus` shows `enforcementheight: 2147483647` | Correct. The enforcing half has no height in this release. |
| Your staking weight rises at 144 888 | The lifted stake-age cap. Coins older than 60 days became eligible. |

## If your node is stuck after the upgrade

1. `getblockchaininfo` — if the tip is below 144 888 and not moving, run `getchaintips` and look
   for a tip marked `invalid` or `conflicting`.
2. A node that rejected a block while running the old binary keeps that block marked invalid after
   you upgrade. `reconsiderblock <hash>` releases it; `reconsiderblock <hash> true` if it was
   marked conflicting.
3. If that does not settle it, `-reindex` rebuilds from your local block files and downloads
   nothing.

## Where to get it, and how to verify it

The binaries are published as a release of the project repository:

**https://github.com/defcon-project/defcon/releases/tag/v23.0.0**

Three archives, under the same names as the previous release:

| File | Contents |
|---|---|
| `defcon-linux.tgz` | Linux x86-64 |
| `defcon-windows.zip` | Windows x86-64 |
| `defcon-apple.tgz` | macOS Intel x86-64 |

Checksums are published beside them: `SHA256SUMS` for the archives, `SHA256SUMS.binaries` for each
binary inside them, and `SHA256SUMS_apple` as in the previous release. **The same archive hashes are
printed at the end of these notes**, so the checksum file and the release text can be checked
against each other.

```
# Linux
sha256sum -c SHA256SUMS
# macOS
shasum -a 256 -c SHA256SUMS
# Windows, PowerShell: prints True when the file matches
(Get-FileHash defcon-windows.zip -Algorithm SHA256).Hash -eq 'c89e550ba2d5b5f1c75c8fd06df177b1625b60468c09f0432d307bc3192abfa1'
```

**There is no detached signature**, as there was none for v22.1.4. The checksums show that a
download arrived intact. They do not show where it came from: `SHA256SUMS` and the hashes printed
below are published on the same page as the archives. Take them from the release page itself, not
from a copy someone sent you.

**The macOS binaries are unsigned and not notarized.** macOS marks anything downloaded through a
browser and will refuse to open an unsigned binary; clearing that mark
(`xattr -d com.apple.quarantine defcond`) or approving it in System Settings is required. They are
cross-built on Linux, and they were started on a real Intel Mac running macOS 15: the checksums
matched, the quarantine mark was set and cleared exactly as above, and the node ran and reported
the scheduled Sentinel height correctly. They target macOS 11.0 and later, on Intel; there is no
Apple Silicon build in this release.

### Checking what you are running

The release reports **23.0.0** and speaks protocol **70242** (`defcond -version`, and
`getnetworkinfo` for the protocol version). The version string carries no height, so **identify the
release by the commit hash of the release build and by the published binary hashes**, not by the
version string. Builds made from the public branch before the release also report 23.0.0.

A tag name alone is not enough either: `v23.0.0` is also the name of a Dash Core release, so in any
clone that tracks a Dash remote it resolves to a different commit.

To check that your daemon really carries the activation height, ask the daemon rather than its
configuration file: `defcon-cli dslstatus` reports `activationheight`, which on this release reads
**145 464** on mainnet. It answers from the first start after the upgrade, long before anything is
active. No RPC reports the activation height itself.

---

## What has been measured, and what has not

This release has been exercised as follows.

**Measured:**

- The unit test gate on the release commit: 151 suites, 676 cases, green — run twice, once by the
  author and once by an independent reviewer with their own build.
- Three hand edits of the activation height, one at a time: each was caught, either by the tests
  failing by name or by the node refusing to start. The tree was restored byte-identical.
- On an offline copy of a real v22.1.4 mainnet data directory, tip 129 777: the in-place upgrade
  reached the same tip and hash; a full rebuild with `-reindex -assumevalid=0` reached that same
  tip independently; a level-4 verification of the last 2 000 blocks found no inconsistency.
  **That snapshot ends below the activation height, so it says nothing about the chain at or after
  it.**
- A regtest rehearsal of the activation on the quorum side, with 65 masternodes: at the test
  height ChainLocks moved to Q60, InstantSend signed on Q60, the pause held through the window and
  ended at the height, the Sentinel layer's first epoch and first commitment behaved as described,
  and a restart with `-reindex` reproduced all of it.
- A regtest rehearsal of the proof-of-stake side: blocks either side of the test height were
  produced and accepted, fee transactions landed where expected, and the stake modifier changed
  rule exactly at the height.
- A regtest rehearsal of the quorum side and the proof-of-stake side together, on one chain of 65
  masternodes and one staker. The author ran it once, and an independent reviewer repeated it on
  their own build of the release commit. Proof-of-stake blocks kept being produced and accepted
  across the test height, with the masternode payment checked either side of it. ChainLocks and
  InstantSend moved to Q60, and the first Sentinel commitment was mined in a proof-of-stake block.
- The same rehearsal again on the release's own schedule: the Sentinel layer starting 576 blocks
  after the activation, and twenty-three Q60 quorums forming on staked blocks in between, each
  waited for phase by phase. ChainLocks and an InstantSend lock moved onto those quorums as the ones
  formed on mined blocks left the active set, the first Sentinel commitment was mined in a staked
  block, no masternode took a penalty, and the staking node re-validated the whole chain with
  `-reindex` and went on producing blocks. It ran on one machine with one staker; an independent
  reviewer reviewed it and reproduced one of its negative controls on their own build.
- On an isolated copy of mainnet at height 139 554, with the release commit and only the activation
  height moved, the chain was carried 200 blocks across that height: blocks kept being produced and
  accepted, a coin older than the old 60-day cap staked a block after the height, the four nodes
  ended on the same tip and the same UTXO set, and `-reindex -assumevalid=0` with a level-4
  verification reached the same tip. An independent reviewer repeated those end-state checks from
  a preserved copy. The record:
  [`doc/v23-activation-on-mainnet-copy.md`](https://github.com/defcon-project/defcon/blob/v22.1.x/doc/v23-activation-on-mainnet-copy.md).
- In the same run, v22.1.4 refused to start on a data directory the new binary had opened ("Error
  upgrading Evo database"). Given `-reindex` on another copy, it rebuilt the shared history and
  rejected the first block of the Q60 mining window, which on mainnet is block 144 778.
- The release binaries start on Debian 13, on Windows 11 and on macOS 15 (Intel), and each reports
  the scheduled Sentinel height correctly. On macOS this was done against the published archive,
  after verifying its checksum and after setting and clearing the quarantine mark a browser
  download applies.
- The proof-of-stake rules have been running gated on the devnet since late August and early
  September 2026.

**Not measured, and stated plainly:**

- **No real v22.1.4 node has been watched meeting upgraded peers.** What the protocol floor does is
  read from the code and exercised in a regtest test, not observed on the live network.
- **There is no Apple Silicon build.** The macOS binaries are Intel and were started on an Intel
  Mac; on an Apple Silicon machine they would need Rosetta, which was not tested.
- The lifted stake-age cap has been observed once, in the mainnet-copy run above: one coin older
  than 60 days staked one block after the height. The devnet cannot show it yet, because its chain
  is younger than its 60-day cap, so the systematic cover remains the unit tests with a negative
  control.

---

## Checksums

```
SHA256 (archives)
18341a5e14b26fee4c20e1eb8f63f4ef3062b6e14cb87f96a8e2fe8beb0c6df2  defcon-linux.tgz
c89e550ba2d5b5f1c75c8fd06df177b1625b60468c09f0432d307bc3192abfa1  defcon-windows.zip
f70ebe96dc400d53bfed18512d5bb720516dc41f7604e5bdf92c37011671d80d  defcon-apple.tgz

Built from commit a06830fd4281a1da989bb788fdd427566060d663
```
---

