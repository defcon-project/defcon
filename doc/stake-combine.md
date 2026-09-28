# Combining wallet outputs

There are two independent ways to combine outputs.

## Combining while staking

With `-stakecombine=1` (the default), a winning coinstake can spend up to 19
additional mature, unlocked outputs paid to the winning key. It does not
combine outputs across different wallets or different keys. Masternode
collateral amounts are excluded. The additional inputs do not increase the
winning kernel's weight; they are selected after the kernel is found.

The combined value matures again with the coinstake. `-staketarget=<amount>`
sets the desired layout size, within network limits. The automatic target is
an estimate based on network weight, not a guaranteed resting-value limit.
Already stakeable inputs are only combined up to that target. For example,
two 12,000 outputs are not combined when the target is 20,000.

`-stakecombine=0` disables additional inputs. Target-based splitting remains
enabled. These startup options apply to every wallet loaded by the process.

The transaction list shows **Staked (combined)** when a coinstake actually
contains more than one input. Its details show the input count, the number
of positive-value outputs, and the net output-count reduction (which can be
negative if splitting creates more outputs). The status column still tells
you whether the transaction is confirmed or accepted.

An address-book label such as `combined` is independent of this information.
It can appear beside an ordinary one-input stake and may be absent beside a
multi-input stake. Renaming a label does not affect staking or combination.

`getstakinginfo` reports `stake_combine`, `stake_target`, and `stake_outputs`
for each wallet. `stake_outputs` counts spendable outputs, including amounts
that cannot stake. The `excluded` fields explain staking exclusions by value.
With POS logging enabled, coinstake construction logs include the wallet
name, candidate count, and selection reason. These are construction attempts,
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
