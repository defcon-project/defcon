# DeFCoN mainnet fork evidence: height 96686

This package records the July 2026 DeFCoN mainnet divergence and the canonical
anchor selected for the v22.1.4 recovery release. It is designed to be public,
machine-readable, reproducible, and cryptographically signed.

## Findings

The last shared block is:

- height: `96685`
- hash: `c826c164126f4193c580992a80fa84d587a4069cab5d848c16148b91c84f8b52`

Two different children of that block exist at height `96686`:

- project-designated canonical branch:
  `6da5b2f350c9c7db51fa3e3594c5c15261c71348da8d972df637e3c398fbda10`
- competing historical branch:
  `47c68500b4d914ec9b187ecfb30506b70f782b0192af0f0b26a54d7f83184396`

Both headers reference the same block at height 96685. This proves that the
first differing height is exactly 96686.

Six independently queried project nodes reported the same hashes at heights
96685, 96686, and 103536 when this package was collected. See
`rpc/independent-node-observations.json`.

## v22.1.4 recovery anchor

The recovery release anchors the selected chain at:

- height: `103536`
- hash: `7b9b767a13d10f2ffa012f8ca05aa80ad9df0719ae3f47cd66251c2a2044d863`
- chainwork: `00000000000000000000000000000000000000000000021fa68e72c9b84c57ac`
- enforcement height: `113117`

Height 113117 is the deterministic consensus trigger. It provides an estimated
10-day upgrade window from mainnet height 107357. Calendar estimates are
informational only because actual block production time varies.

## Evidence boundaries

This package proves:

- the common ancestor at height 96685;
- two different block hashes at height 96686;
- the exact headers observed for both branches;
- agreement of six queried nodes on the selected branch and recovery anchor;
- the integrity of the published files under the included signing key.

The signature does not independently decide which branch a community must
select. The canonical designation is the project decision encoded by v22.1.4.
The `chainlock` fields are RPC observations made by nodes on the selected active
chain at collection time; they are not presented as a complete archive of every
historical CLSIG exchanged by every partition.

## Contents

- `manifest.json`: canonical machine-readable summary.
- `rpc/block-headers.json`: relevant block-header RPC results.
- `rpc/independent-node-observations.json`: six-node hash comparison.
- `logs/seed1-fork-excerpt.log`: minimal public log excerpt showing both branches.
- `verify_fork_structure.py`: parses the raw headers and verifies that
  both height-96686 blocks share the same height-96685 parent.
- `SHA256SUMS`: hashes of every signed payload file.
- `SHA256SUMS.sig`: Ed25519 signature over `SHA256SUMS`.
- `PUBLIC_KEY.pub`: public signing key.
- `SIGNING_KEY_FINGERPRINT.txt`: OpenSSH SHA256 key fingerprint.
- `ALLOWED_SIGNERS`: verifier identity and namespace binding.
- `VERIFY.md`: offline verification procedure.

No RPC credentials, SSH credentials, private keys, wallet material, or private
API tokens are included.
