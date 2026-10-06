# Continuous staking consolidation

The staking wallet gradually consolidates outputs during successful coinstakes.
There are no wallet UTXO-count thresholds and no guarantee of staying below a
particular count. Locked wallets, wallets without an eligible winning input,
locked coins and masternode collateral are not automatically repaired.

## Defaults and privacy

`-stakecombine=1` enables consolidation. `-stakecombinescope=wallet` is the default:
spendable P2PK/P2PKH outputs from different keys of the SAME wallet can join a
winning input. This links those source addresses on chain. `-stakecombinescope=key`
restricts gathering to the winning key. `-stakecombine=0` disables gathering and
uses the original split policy. Neither mode sends independent maintenance
transactions. No funds are taken from a different loaded wallet.

## Policy

The base target is still derived from the existing 72-stake network-weight
estimate, unless `-staketarget` overrides it. Continuous mode aims for twice that
base size and starts splitting at three times the base, bounded by staking amount
limits. Safe amounts and collateral avoidance take precedence. These are wallet
policy factors, not consensus changes or an optimal-yield guarantee.

A bounded smallest-first search evaluates complete input/output layouts INCLUDING
the reward. Optional inputs are selected only for an actual negative UTXO delta
and a strict improvement over the kernel-only layout. Ties keep the cheaper
prefix. A coinstake still uses at most 20 inputs. The kernel is always input 0;
all outputs pay its own key. Signature and serialized block-space checks remain
mandatory. If optional inputs fail to sign or fit, construction retries the kernel
alone; if necessary the output layout is reduced while retaining amount safety.

Optional eligible inputs consume at most one base target per win. They must also
fit the remaining 5% wallet resting-value budget after accounting for the kernel
and currently immature/too-young value. Below-minimum fragments consume no
eligible-value budget. This budget limits OPTIONAL consolidation; it cannot stop
the unavoidable rest of a winning input, and small wallets may have no headroom.
Larger stable outputs can increase the amount resting after wins compared with
the previous policy. Actual long-term reward impact must be measured separately.

## User-visible facts

The transaction details show the winning input separately from additional inputs,
source addresses and current labels, exact amounts and previous outpoints, new
outputs and their addresses, principal, net minted reward and signed UTXO-count
change. Source transactions are read from wallet history, so already-spent inputs
remain explainable. Missing parents are shown as unknown; totals and reward are
not invented. Unconfirmed/orphaned transactions are explicitly not described as a
confirmed consolidation. Counts describe that transaction, not the current total
wallet count. Labels reflect the current address book, not a historical snapshot.

Linux RPC: `getstakinginfo` exposes the scope, compact target, split threshold and
rest-budget policy. `gettransaction` adds `stake_details` for coinstakes, including
input/output arrays, confirmed state, signed `utxo_delta`, and principal/net reward
only when all input records are available.

## Validation

Unit coverage checks conservation, amount/collateral limits, bounded input count,
rest budget, target-size inputs, stable later wins, unknown GUI input records and
HTML escaping. Functional coverage exercises default cross-key and key-only scope,
legacy/descriptor wallets, excluded coins, positive net reward, disconnect/reconnect,
and observer reindex. An unchanged observer binary can be supplied with
`feature_pos_stake_combine.py --observer-binary=/absolute/path/to/defcond`.

These tests do not establish a weeks-long mainnet UTXO trend or future staking
returns. Review measured counts and resting stake weight together.
