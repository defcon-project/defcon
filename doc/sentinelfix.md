# Sentinel processing: keeping blocks and quorum key generation on time (SF1)

This is the design note behind three changes that ship in v23.1.0 under the name SF1.

| Step | Pull request | What it does |
| --- | --- | --- |
| SF1-0 | #387 | Measures where a block's processing waits, and adds a test switch. No change in behaviour. |
| SF1-A | #390 | Runs the Sentinel tick behind the DKG notification of the same block event, and drops byte-identical copies of a liveness announcement without decoding them. |
| SF1-B | #393 | Keeps an exact table of which peer already has which Sentinel message, so that a node does not send a message back to a peer that has it. |

None of them changes consensus or the peer-to-peer protocol (section 5).

Comments in the source name this note as `sentinelfix.md <section>`, with the item of a numbered
list after a slash: `sentinelfix.md 4.3/5` is item 5 of section 4.3. The section and item numbers
below are kept stable for that reason. The note describes the design the changes were built from.
Where the code differs from the letter of the note, the comment at that place says so, and the
code is what holds.

Terms used here. The Sentinel layer (DSL in the source) works in epochs of 24 blocks; a block's
*position* is its height modulo 24. Its messages are the liveness announcement (`POSERESPONSE`),
the report (`POSEREPORT`), the commitment (`POSECOMMIT`) and the challenge (`POSECHALLENGE`). The
*tick* is the Sentinel work a node does for each new chain tip. DKG is the distributed key
generation in which a quorum forms.

## 1. In short

- **The symptom.** On mainnet, under v23.0.0, masternodes on small machines that were in sync were
  penalised again and again (PoSe) for missing the DKG of the 60-member ChainLock quorum
  (`llmq_defcon`).
- **What the measurement shows** (section 2). On such a node blocks arrive late from position 18
  of the 24-block cycle to position 9 of the next one, and hardly ever in between. The DKG starts
  at position 0: positions 0 and 1 are its initialisation, 2 and 3 the contribution. A node that
  connects the start block minutes late initialises after the contribution phase and is marked as
  not having taken part.
- **The likely mechanism, from the code** (section 3). Sentinel reports flood the network from
  position 18 and liveness announcements at position 0. Every copy was decoded on the one thread
  that handles all peer messages, so block messages queued behind them, and the Sentinel tick ran
  on the validation queue ahead of the DKG notification of the same event. The measurement shows a
  strong cyclic pattern; it does not rule out local disk and memory pressure or the peers, which
  is why section 6.3 measures cause and effect in a lab.
- **The fix**, in three steps: measure first (SF1-0, section 4.1); move the tick behind the DKG
  notification and stop decoding copies (SF1-A, 4.2); stop sending a message back to a peer that
  has it (SF1-B, 4.3). A separate worker thread and other larger steps were left out unless the
  lab still showed congestion (4.4).

## 2. Measurement

The source is the logs of two masternodes of the same small size (1 vCPU, 1 GB of memory): about
nine days of one that was penalised repeatedly and twelve days of one that was not. The delay of
a block is the time its connection was logged minus the block's timestamp. The first three rows
count the heights both logs cover; the others count each node's own log.

| | penalised node | other node |
| --- | --- | --- |
| Blocks connected more than 120 s late (the same 4,946 heights) | 695 | 411 |
| Blocks connected more than 300 s late | 302 | 163 |
| DKG start blocks (height modulo 24 = 0) more than 180 s late | 92 of 206 | 38 of 206 |
| `llmq_defcon` rounds the node initialised after the contribution phase | 14 of 201 | 5 of 261 |
| From connecting the start block to the DKG initialisation succeeding, median and 90th percentile | 38 s and 287 s | 23 s and 37 s |
| PoSe penalties | 12 | 3 |

Blocks connected more than 120 s late, by position (about 206 blocks fall on each position):

| height modulo 24 | penalised node | other node |
| --- | --- | --- |
| 18 to 23 | 370 | 252 |
| 0 to 5 | 306 | 155 |
| 6 to 9 | 18 | 4 |
| 10 to 17 | 1 | 0 |

Limits of the measurement: two nodes; the fourth row is read off the log, not off the DKG handler:
a round counts as initialised after the contribution phase when the tip update logged just before
the initialisation succeeded was at least four blocks past the start block; the interval in the
fifth row is not pure queueing, it also holds the validation queue, the callbacks ahead of the DKG
notification, the DKG thread's scheduling and the initialisation itself; a block's timestamp
comes from the staker's clock; and the logs carry no message queue lengths.

## 3. The code before the change

1. **Epoch and position.** `nDSLEpochInterval` is 24; the tick works from `height % 24`.
2. **Sentinel work in the cycle.** At the start of every tick the node enters the epoch and takes
   up the liveness announcements held back for it, checking their signatures and relaying them.
   Once in the epoch it signs and relays its own announcement. From position 18 it signs, takes in
   and relays its own reports. From position 21 it assembles the commitment and asks the quorum to
   sign it, waiting until position 23 if its pool is incomplete. A commitment is valid only in the
   boundary block, at height `(E + 1) * 24`.
3. **The tick ran on the validation queue, ahead of the DKG notification.** `PeerManager`'s
   `UpdatedBlockTip` called the tick directly. Validation events run on one background thread, one
   event at a time, their subscribers in the order of registration, and `PeerManager` is
   registered before the interface that notifies the DKG handler. A function added with
   `CallFunctionInValidationInterfaceQueue` goes to the end of the queue, and
   `SyncWithValidationInterfaceQueue` waits only for what was queued before it. Between
   `PeerManager`'s callback and the end of the event only that later interface can change what
   the tick reads: it first updates the masternode sync, which in one state steps back to its
   blockchain stage, the very state the tick's entry condition reads.
4. **DKG phases** come from the blocks since the start block: `(height - start) / dkgPhaseBlocks
   + 1`. For `llmq_defcon` (`dkgPhaseBlocks` 2) initialisation is positions 0 and 1, contribution
   2 and 3, complaints 4 and 5.
5. **Incoming Sentinel messages are handled on the message thread**, one received message per
   call, so a peer's block message is reached only after what that peer queued before it. A
   report is 165 bytes: a byte-identical copy of one already held is dropped without decoding
   (#304), anything else is decoded (a G2 point with its subgroup check), taken in and relayed. A
   liveness announcement is a fixed 132 bytes: it was decoded first, and only then recognised as
   a copy. A message that cannot be read scores against its sender.
6. **Whole-message flooding.** A Sentinel message is relayed whole to every connected peer that
   is not blocks-only, except the one it came from. There was no record of who has what, so a
   node received the same message from many peers.
7. **#304 was not in a release yet.** It is in v23.1.0, not in v23.0.0.
8. **The commitment is signed by the ChainLock quorum**: on mainnet `llmq_defcon` (60 members, at
   least 44, threshold 41).
9. **Regtest.** The regtest ChainLock quorum is the three-member `llmq_test`, with the same
   timing (24 blocks, two-block phases); `llmq_defcon` forms there only with
   `-testactivationheight=chainlocksv2@<height>` and at least 44 masternodes.
10. **Meters.** `-dslperf` logs, per epoch, a count and a total time for each metric, without
    percentiles. `TIP_QUEUE` measures the wait in the validation queue until an event's callbacks
    start.

## 4. The changes

### 4.1 SF1-0: measurement and a test switch (no change in behaviour)

1. **A monotonic arrival time.** A received message carries a second arrival time
   (`m_time_steady`) taken from the steady clock. The existing, mockable arrival time and all its
   uses stay as they are.
2. **Delay metrics by epoch and position**, all on the steady clock: `TIP_QUEUE` (existing);
   `PEERMAN_TIP_RUN`, the run of `PeerManager`'s `UpdatedBlockTip`; `DKG_TIP_START`, from the
   event being queued to the DKG notification starting on it; `DSL_TICK_RUN`, the run of the
   Sentinel tick; `BLOCK_MSG_WAIT`, a block, compact block, block transactions or headers message
   waiting from its arrival until its processing starts.
3. **The position.** For the first four it is the new tip's height modulo 24. For
   `BLOCK_MSG_WAIT` it is the active chain's height plus one, modulo 24, when processing starts:
   the place of the next block.
4. **Storage.** In the existing bounded per-epoch slots, 40 log2 buckets for each metric and
   position: bucket k holds [2^k, 2^(k+1)) nanoseconds for k from 10 to 49, with an overflow
   count. When every epoch slot is taken the sample is left out and counted.
5. **Output.** The epoch summary writes one line for each metric and non-empty position.
   `contrib/devtools/dsl_perf_pct.py` computes the 50th, 90th and 99th percentile by position,
   conservatively: the upper edge of the bucket the sample falls in.
6. **New counters:** `DECODE_RESPONSE` (a liveness announcement decoded) and
   `DUPLICATE_RESPONSE_UNDECODED` (a copy dropped without decoding; it counts from SF1-A).
7. **A test switch,** `-dsltestprocessdelay=<ms>`, on regtest and devnet only (startup is refused
   elsewhere). The node waits that long after reading each report or liveness announcement, after
   checking each held announcement in the tick, and after signing each of its own reports. It
   does not wait for a copy dropped without decoding. The switch therefore follows the real
   shape of the cost: where the code decodes it is slow, and where the fix no longer decodes it
   is not.

### 4.2 SF1-A: the tick behind the DKG notification, copies without decoding

1. **The tick at the end of the event.** The validation interface gets a second method for the
   end of the tip event, `UpdatedBlockTipFinal`, empty by default. After the existing round of
   callbacks the event runs a second round for it, in the same event and on the same thread.
   - At the point where the tick used to run, `PeerManager` no longer runs it. It notes the new
     tip and the state of the masternode sync as it stands at that point.
   - `PeerManager`'s end-of-event method runs the tick for that event with the noted sync state:
     the tick's entry condition uses it instead of reading the sync again, because a later
     subscriber of the same event can reset the sync. Every other check of the tick, the active
     base and a reorganisation among them, is made when it runs, as before.
   - So the DKG notification runs before the tick, and the tick is still part of the same event.
     Whatever waits for the event (`SyncWithValidationInterfaceQueue`, the brake of chain
     activation on a long queue, the `syncwithvalidationinterfacequeue` call of the tests) waits
     for the tick too.
   - The body of the tick and its inner order do not change. The ticks of successive events run
     in chain order, since each event ends with its own tick. During initial block download
     nothing is noted and no tick runs, as before.
2. **A copy of a liveness announcement is not decoded,** on two branches, after the pattern of
   #304.
   - *An accepted announcement.* For each one the manager keeps `(epoch, base hash, proTxHash)`
     with the SHA-256 of the 132 bytes. The message thread reads the epoch and the proTxHash off
     the fixed layout and hashes the bytes; if the base is connected and the same triple holds the
     same hash, the copy is dropped without decoding.
   - *A held announcement.* A held entry keeps the hash of its own bytes. A byte-identical copy
     adds its sender as a voucher without decoding, by the same rule as before.
   - Anything that is not byte-identical takes the ordinary path. The stored hashes go with the
     announcements they belong to, epoch by epoch.
3. **Why no separate worker thread here.** The measured causes are covered: copies of reports by
   #304, copies of announcements by item 2, the order of the tick and the DKG by item 1. What is
   left is one decode and one check for each different message, in the order they arrive. A
   worker thread brings risks of its own (a different signed hash, a hand-over gap, an unbounded
   queue, lost scoring), so it comes only if the lab shows that the rest still reaches into the
   DKG window (4.4).

### 4.3 SF1-B: an exact seen table

1. **Structure.** One table for the node. The key is the SHA-256 of the message type and its
   exact serialized payload; the value is the message's epoch and a bitmap of peer slots (as
   built, also a mark that this node has relayed the message). A peer gets a slot when it
   connects. When it disconnects the slot's bit is cleared from every entry before the slot can
   be given out again.
2. **Noting.** A bit is set when the peer actually sent this node the message (a copy dropped
   without decoding counts too), or when this node actually sent it to the peer. A received
   message is noted only for an epoch the table keeps, which is the tip's, the one before and the
   next, and only if it is no longer than a relay of its type can be.
3. **Relaying.** A relay leaves out a peer whose bit is set. In every other case it sends.
4. **The invariant.** The table withholds a relay only for an exactly matching message that was
   actually received from or sent to that peer. A full table or an expired entry can cause an
   extra send at most; it can never suppress a first delivery. A probabilistic filter (Bloom)
   cannot be used for this.
5. **Expiry and size.** Entries older than the epoch before the tip's are dropped. The cap is
   four times the honest message ceiling of an epoch (`DSLHonestEpochCeiling`); when the table is
   full, the entries of the oldest epoch go. An entry is about 80 bytes: with 218 masternodes and
   two epochs that is about 3,500 entries, about 0.3 MB.
6. It covers every Sentinel message type that is relayed, the commitment included.

### 4.4 SF1-C: only if the lab asks for it

If a required condition of the lab runs b, b2 or c (6.3) fails, SF1 stays open and a separate
design follows from the measurement. The choices, by what the measurement shows: a separate
worker thread for the different messages; serializing a relayed message once instead of once for
each peer; spreading out the relay of a node's own reports; a timer that follows a signature
recovered after the last tick (an inherited gap, section 7). Each would come with its own design
note. The acceptance conditions of the lab did not require them (section 8), so they are left for
after v23.1.0.

## 5. Invariants

1. **Consensus.** The format, validity and activation of the commitment, the choice of the
   signing quorum, the length of the epoch and the roles of positions 18, 21 and 23 do not change.
2. **Peer-to-peer contract and serialization.** The same messages with the same encoding; no new
   message type and no protocol version bump. v23.0.0 nodes interoperate as before.
3. **Place and order of Sentinel processing.** The tick's body runs on the same serial thread, in
   the same inner order and in the same validation event as before, only behind the event's other
   callbacks; whether it may run is decided where it used to run. Incoming Sentinel messages are
   handled on the message thread in the order they arrive. There is no new queue, no new thread
   and no new state of lagging behind.
4. **Scoring.** Dropping without decoding applies only to a message that is byte-identical to one
   already decoded (accepted or held). The same bytes would decode the same way, so a message
   that cannot be read cannot escape scoring this way.
5. **Memory.** What SF1-A stores is tied to the accepted announcements and the held entries and
   goes epoch by epoch. The table of SF1-B is bounded (4.3/5).
6. **The seen table is exact** (4.3/4).
7. **Shutdown.** The tick is part of its event; no function is queued separately. The queue is
   drained before `PeerManager` goes away.
8. **Log lines.** The existing `DSL --` log lines keep their text, because functional tests read
   them.

## 6. Tests and acceptance

### 6.1 Unit tests

- **The tick at the end of the event.** A test subscriber registered after `PeerManager` runs
  before the Sentinel tick. With two events and a sync marker queued ahead, the sync call returns
  only when both ticks have run, in chain order. On a reorganisation the tick checks the active
  base when it runs. During initial block download there is no tick.
- **The sync state.** A test subscriber resets the masternode sync to its blockchain stage in the
  middle of the event. The tick still runs with the noted state, and in the event of the epoch's
  last block (position 23) the commitment is relayed with the signature the quorum produced, as
  before the change, when the tick ran ahead of the reset. A variant that reads the sync afresh
  fails this test.
- **Copies of a liveness announcement.** Byte-identical, accepted, same base: not decoded. Other
  bytes or another base: decoded. On the held branch a byte-identical copy adds its sender as a
  voucher, and the rules of vouching do not change. A malformed message never matches, and is
  scored as before.
- **Measurement.** The position a sample is booked to; the edges of the buckets and the overflow;
  with the mock clock fixed, a real wait of 50 ms measures as at least 50 ms in `BLOCK_MSG_WAIT`.
- **The seen table.** A message still gets through over a single connection with the table full
  or expired; a full table and an expired entry cause extra sends only, and an entry expires with
  its epoch; after a disconnect the slot's bit is cleared.

### 6.2 Functional test, in the hosted gate

`feature_dsl_dkg_isolation.py`: regtest, seven masternodes and a block producer, the Sentinel
layer active from the first block. The ChainLock and DKG quorum is the three-member `llmq_test`,
whose timing is that of `llmq_defcon` (section 3, item 9). One masternode, D, runs with
`-dsltestprocessdelay=200 -dslperf`.

1. **A flood of copies, with a deterministic sync point.** One connection captures a liveness
   announcement that D has already accepted. Another sends D 50 byte-identical copies of it and
   then a ping; the pong comes only when everything sent before it has been handled. Expected:
   the pong within 2 s, D's `DECODE_RESPONSE` does not grow for the copies, and the connection is
   neither scored nor dropped. Decoded one by one the copies alone take 50 times 0.2 s.
2. **The order of the tick.** D produces the block at position 18 itself, so the signing of its
   own reports falls into that block's tick. Expected: the 99th percentile of D's `DKG_TIP_START`
   at position 18 is at most 0.25 s. With the tick ahead of the DKG notification it is the
   signing time of the reports at least.
3. **The commitment does not get worse.** For three epochs D is a valid member of the quorum that
   signs the commitment (the test picks it from `quorum info` once the quorum exists). Expected:
   every boundary block carries a commitment with the same hash on every node, and D is observed
   in it, not missed.
4. **Shutdown with a tick pending.** D is stopped right after a block. Expected: a clean stop
   with no error in the log.

The negative control: on the tree of SF1-0 (the base with the meter and the test switch) the test
fails at step 1, where the 50 copies took 10.1 s, and at step 2, where the DKG notification
waited between 1.07 and 2.15 s (one bucket of the meter). With SF1-A it passes: the pong came
back after 0.06 s and the DKG notification started within 0.3 ms.

### 6.3 The 60-member lab

Regtest with 65 masternodes and a block producer, so that `llmq_defcon` (60 members, at least 44,
threshold 41) forms and signs the commitment; one block every 10 seconds; the slowed masternodes
run with `-dsltestprocessdelay=15 -dslperf`. The 15 ms stand for a weak server: about five times
the decode and check cost of one message measured earlier.

- **Runs.** a) the tree of SF1-0 with 5 slowed nodes, the negative control, valid only if the
  congestion shows; b) the fix with the same setup; b2) the same with 25 slowed nodes, so that a
  quorum forms only if slowed members contribute validly, with the same run on the tree of SF1-0
  for comparison; c) mixed versions, 20 masternodes without the fix and the rest with it.
- **Acceptance in b and b2.** Every `llmq_defcon` round forms; every membership of a slowed node
  is valid (`quorum info`); no slowed node is penalised; the 99th percentile of `DKG_TIP_START` is
  at most 2 s at every position; the 99th percentile of the slowed nodes' tip delay at positions
  22, 23 and 0 to 5 is at most 2 s, the delay being the node's tip update minus the producer
  starting to create that block, on one real clock; every boundary block carries a commitment
  with the same hash on every node; after the warm-up epochs no live masternode is marked as
  missed.
- **Acceptance in c.** Every round forms, every boundary block carries a commitment, and no live
  masternode is marked as missed.
- **The fifth condition was replaced once.** It first read on `BLOCK_MSG_WAIT`. That meter books
  every block-announcing message to the position the chain expects next, so it also counts copies
  of blocks already connected that are handled late; the runs with the fix failed on that one
  condition while the tip delay at the judged positions stayed within the limit. The tip delay
  measures what the condition was meant to: when a slowed node connects the block.
  `BLOCK_MSG_WAIT` stays as a diagnostic.

### 6.4 Devnet

Not part of this work.

## 7. What is not part of this, and why

- **Announcing by hash and fetching on request.** It would cut the traffic most, and it is a
  change of the peer-to-peer protocol, with a version bump and a mixed-network transition. After
  v23.1.0, if measurement calls for it.
- **Shifting the epoch away from the DKG start, or longer DKG phases.** A consensus change with
  an activation height.
- **Swapping the registration order of `PeerManager` and the DKG notification.** It would change
  the order of every callback, block announcement among them. The end-of-event round of 4.2/1
  moves only the tick.
- **Batched BLS verification.** Decoding, not signature checking, was the main cost in an earlier
  measurement.
- **An inherited gap:** a signature recovered after the last tick, at position 23, has nothing to
  relay it. The block producer can still include the commitment from its own pool. SF1 does not
  make this worse.

## 8. Result

The three steps were merged in the order SF1-0 (#387), SF1-A (#390), SF1-B (#393).

In the lab the negative control showed the congestion: with five slowed nodes on the tree of
SF1-0 the 99th percentile of their tip delay reached 66.9 s at position 1, none of their 17
memberships was valid, one round in 30 did not form, and slowed nodes were penalised. With SF1-A
and SF1-B together every round formed, every membership of a slowed node was valid and none was
penalised, both with five slowed nodes (60 rounds, 280 memberships) and with 25 (60 rounds, 1,373
memberships); the 99th percentile of the tip delay at the judged positions was at most 1.19 s
and 0.60 s. With the same 25 slowed nodes on the tree of SF1-0 no round formed in 30. The
mixed-version run met its conditions (30 rounds, 135 memberships).

One thing the fix did not change: after the wave of reports, at positions 19 to 21, the 99th
percentile of a slowed node's tip delay is between 6 and 27 s, with and without the fix. That is
the area of the steps in 4.4. The acceptance conditions did not require them, and they are left
for after v23.1.0.

SF1 was closed with a fourth pull request, #408, which makes Sentinel reports follow the epoch's
own masternode list.

One of the steps in 4.4 has been taken since, without a change to any message: a relayed
Sentinel message is serialized once, and each recipient is sent a copy of those bytes. The report
store likewise keeps the bytes of a held report's signature, so a copy that arrives later is
compared with them and nothing is serialized for it. `-dslperf` now also gives the time spent on
the copies dropped unread (`duplicate_undecoded`) and counts the signatures the store kept bytes
for (`store_sig_bytes`).
