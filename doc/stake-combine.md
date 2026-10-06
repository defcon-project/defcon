# Combining wallet outputs

There are two independent ways to combine outputs.

## Combining while staking

With `-stakecombine=1` (the default), a winning coinstake can spend up to 19
additional eligible, unlocked outputs from the SAME wallet. The default
`-stakecombinescope=wallet` permits different own keys and links those source
addresses on chain. `-stakecombinescope=key` restricts gathering to the winning
key. Masternode collateral amounts are excluded. Additional inputs are selected
after the kernel is found and do not increase its winning weight.

The wallet evaluates inputs and new outputs together, including the reward.
Optional inputs must reduce the UTXO count and improve on the kernel-only layout.
There is no wallet UTXO-count activation threshold or guaranteed maximum count.
The base target comes from network weight or `-staketarget=<amount>`. Continuous
mode aims for twice that base and normally splits at three times the base,
subject to staking limits and collateral avoidance. An optional consolidation
budget accounts for already-resting value and the winning input; it can leave
no room to gather additional stakeable coins.

The combined value matures again with the coinstake. `-stakecombine=0` disables
additional inputs and restores the original target-based split policy. Startup
options apply to every wallet loaded by the process. See
[Continuous staking consolidation](staking-consolidation.md) for the complete
size and resting-value policy, exclusions and validation limits.

The transaction list shows **Staked (combined)** when a coinstake actually
contains more than one input. Its details explain the inputs, new outputs,
principal, net minted reward and transaction-level UTXO-count change. Missing
input records and unconfirmed transactions are identified explicitly. The status
column still tells you whether the transaction is confirmed or accepted.

An address-book label such as `combined` is independent of this information.
It can appear beside an ordinary one-input stake and may be absent beside a
multi-input stake. Renaming a label does not affect staking or combination.

`getstakinginfo` reports the combine scope, base target, compact target, split
threshold, resting-value budget and `stake_outputs` for each wallet.
`stake_outputs` counts spendable outputs, including amounts that cannot stake;
the `excluded` fields explain staking exclusions by value. `gettransaction`
includes `stake_details` for coinstakes. POS logs describe construction attempts,
not proof of block acceptance.

## Manual combining

`combineoutputs` defaults to a preview and uses the selected wallet only:

```
defcon-cli -rpcwallet=49m combineoutputs
defcon-cli -rpcwallet=49m -named combineoutputs staking_only=true
```

The preview does not send, reserve inputs, or consume change keys. It reports
the estimated signed size, fees, output values and `stakeable_amounts` for
each batch. A later call selects again from the current wallet state.

After reviewing the preview, `dry_run=false` executes the request:

```
defcon-cli -rpcwallet=49m -named combineoutputs dry_run=false staking_only=true
```

`staking_only=true` requires all output amounts to meet staking limits after
fees. The new outputs still need the required age and confirmations before
staking. With the default `staking_only=false` and `output_size=0`, combining
does not guarantee staking-sized outputs. With a nonzero `output_size`, the
output amounts must also satisfy the staking limits.

Each send request with at least two candidates creates one new address labelled
`combined`; all its batches use that address. Previews create no address. Combining different
addresses links them on-chain. Locked outputs, masternode collateral amounts,
and unsupported scripts are excluded. Each batch pays its own fee, and a lone
last input is left untouched. A requested batch may be too large even when its
input count is within the accepted range.

A send stops on the first failed batch; earlier batches remain sent. Inspect
every batch before retrying. Returned submission states are:

| Status | Meaning |
| --- | --- |
| `submitted` | The node accepted the submission; confirmation is separate. |
| `recorded` | Saved in the wallet, with broadcasting disabled. |
| `rejected_abandoned` | Submission failed explicitly; abandonment succeeded and spendable input indexing was restored. |
| `unknown` | The transaction remains recorded; check its state before retrying. |

The `txid` identifies a recorded transaction and does not by itself prove
network acceptance. No automatic resend follows an uncertain result.
