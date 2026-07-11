# Multisig GUI Hardening Plan

Status: proposed. This document covers Qt wallet UX and wallet-policy hardening only. It does not change consensus rules or require a network activation height.

## Scope

The multisig GUI on `feature/gui-multisig-wallet` provides profile storage, watch-only tracking, PSBT creation, signing, signature merging, and transaction broadcast. The workflow needs a small set of safety and consistency improvements before it is considered suitable for mainnet wallet releases.

## Policy Decisions

The following policy choices should be implemented unless a later review explicitly changes them:

- The multisig GUI requires at least one confirmation for selected UTXOs.
- A user-entered destination equal to the source multisig address is rejected before PSBT creation. Wallet-generated change continues to return to the source multisig address and is displayed as change.
- Explicit validation failures block spending and PSBT construction.
- A missing redeem script in an older local profile remains a warning, not a hard failure, to preserve compatibility with legacy profiles.
- Descriptor wallet detection is fail-closed: an RPC failure or unknown wallet type must not reach the legacy `importmulti` RPC. Descriptor-based watch-only import is a separate future feature.

## Implementation Sequence

### 1. Block Unsafe Multisig Profiles

Commit title:

```
qt: block unsafe multisig spend profiles
```

Introduce one shared profile-spendability check and use it to disable both `Spend` and `Build transaction` when any of the following is true:

- The multisig address is invalid.
- The stored redeem script does not match the stored address.
- The required-signature value is not valid for the number of keys.
- The stored network differs from the running network.

Profiles with an unknown redeem script may still proceed with a visible warning for backward compatibility.

Reject only a user-entered destination equal to the source multisig address before creating a PSBT. Do not reject or reclassify the wallet-generated change output: `changeAddress` intentionally remains the source multisig address and the preview must continue to show that output as change.

Primary files: `src/qt/multisigpage.cpp`, `src/qt/multisigspenddialog.cpp`, and `src/qt/multisigutil.{h,cpp}`.

### 2. Final Broadcast Confirmation and Fee Safety

Commit title:

```
qt: require safe multisig broadcast confirmation
```

Before sending a completed PSBT to the network, finalize it in memory and present a separate confirmation dialog containing:

- Every transaction output and its decoded destination when available.
- Amount sent.
- Fee and fee-to-payment ratio.
- Change amount and destination when they can be identified.
- Required and collected signatures.
- Transaction ID calculated from the finalized transaction.

`Cancel` must leave the transaction unbroadcast. The subsequent `Broadcast transaction` action must send exactly the finalized transaction that was shown in the confirmation dialog.

Add GUI-side absolute-fee and fee-to-payment-ratio policy checks. A high fee must be shown as a prominent warning and require a separate acknowledgement. A hard upper limit must block broadcasting. The final thresholds must be defined as reviewable policy constants rather than inferred from the node's fee-rate guard.

Never guess missing financial information. If the fee cannot be determined, block normal GUI broadcast. If change cannot be identified, show every output, mark change as unknown, and require a separate explicit acknowledgement. This is especially important for PSBTs loaded without a stored multisig profile.

Primary file: `src/qt/multisigspenddialog.cpp`.

### 3. Harden UTXO, Profile, and Descriptor Handling

Commit title:

```
qt: harden multisig profile and UTXO handling
```

- Require one minimum confirmation for multisig UTXO selection. This first version has no zero-confirmation override.
- Display that only confirmed multisig UTXOs are spendable through this GUI workflow.
- Handle `AddEntry()` failures so a duplicate profile cannot be reported as newly created.
- Normalize public-key hex before duplicate checking in the create dialog. Setup-file import already performs case-insensitive duplicate detection.
- Replace boolean descriptor detection with descriptor, legacy, and unknown states. Unknown is fail-closed.
- Prevent descriptor and unknown wallet states from reaching every `importmulti` call path, including create-only plus `Track address` and setup-file import. Show an explanation that descriptor-based tracking requires a future `importdescriptors` implementation.

Primary files: `src/qt/multisigdialog.cpp`, `src/qt/multisigpage.cpp`, and `src/qt/multisigutil.cpp`.

## Verification

For every implementation commit:

```
git diff --check
make -j$(nproc) src/qt/defcon-qt
```

Perform a manual 2-of-3 multisig test on a non-production environment:

1. Create and fund a 2-of-3 multisig address, then wait for one confirmation.
2. Build a PSBT and verify recipient, amount, fee, and change preview.
3. Verify that a user-entered destination equal to the source multisig address is rejected, while wallet-generated change still returns to that address and is shown as change.
4. Verify that one signature cannot broadcast and two valid signatures can finalize.
5. Verify that cancelling the final confirmation does not place a transaction in the mempool.
6. Verify that the final broadcast shows a transaction ID only after successful broadcast.
7. Verify that invalid address, redeem-script mismatch, invalid M-of-N, and wrong-network profiles cannot spend.
8. Verify that descriptor wallets do not call `importmulti` from any multisig GUI path.
9. Verify duplicate profile and mixed-case duplicate public-key handling.
10. Verify high absolute fee, high fee ratio, unknown fee, and unknown change handling.
11. Simulate descriptor detection RPC failure and verify that no legacy import RPC is attempted.

The repository currently has no automated Qt GUI test coverage for this workflow. Release confidence therefore depends on targeted helper tests plus the complete documented manual 2-of-3 procedure above.

## Release Criteria

The multisig GUI is ready for a mainnet wallet release only when:

- Every broadcast requires a final confirmation.
- Explicitly invalid profiles cannot create or sign a spend PSBT.
- User-entered self-send cannot be misrepresented as wallet-generated change.
- High or unknown fees cannot pass through a routine confirmation flow.
- Unconfirmed UTXOs are not selected.
- Descriptor and unknown wallet states do not invoke unsupported legacy import RPCs.
- The Qt build, targeted tests, and the manual 2-of-3 workflow pass.
