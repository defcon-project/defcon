# Multisig GUI Hardening Plan

Status: proposed. This document covers Qt wallet UX and wallet-policy hardening only. It does not change consensus rules or require a network activation height.

## Scope

The multisig GUI on `feature/gui-multisig-wallet` provides profile storage, watch-only tracking, PSBT creation, signing, signature merging, and transaction broadcast. The workflow needs a small set of safety and consistency improvements before it is considered suitable for mainnet wallet releases.

## Policy Decisions

The following policy choices should be implemented unless a later review explicitly changes them:

- Multisig treasury spending uses at least one confirmation by default.
- Sending to the source multisig address is rejected by the GUI. It must not be displayed as a change output.
- Explicit validation failures block spending and PSBT construction.
- A missing redeem script in an older local profile remains a warning, not a hard failure, to preserve compatibility with legacy profiles.
- Descriptor wallets must not attempt the legacy `importmulti` RPC. Descriptor-based watch-only import is a separate future feature.

## Implementation Sequence

### 1. Final Broadcast Confirmation

Commit title:

```
qt: require final multisig broadcast confirmation
```

Before sending a completed PSBT to the network, finalize it in memory and present a separate confirmation dialog containing:

- Recipient address or addresses.
- Amount sent.
- Fee.
- Change amount and destination.
- Required and collected signatures.
- Transaction ID calculated from the finalized transaction.

`Cancel` must leave the transaction unbroadcast. The subsequent `Broadcast transaction` action must send exactly the finalized transaction that was shown in the confirmation dialog.

Primary file: `src/qt/multisigspenddialog.cpp`.

### 2. Block Unsafe Multisig Profiles

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

Reject a destination address equal to the source multisig address before creating a PSBT.

Primary files: `src/qt/multisigpage.cpp`, `src/qt/multisigspenddialog.cpp`, and `src/qt/multisigutil.{h,cpp}`.

### 3. Harden UTXO, Profile, and Descriptor Handling

Commit title:

```
qt: harden multisig profile and UTXO handling
```

- Change multisig UTXO selection from zero to one minimum confirmation.
- Display that only confirmed multisig UTXOs are spendable through this GUI workflow.
- Handle `AddEntry()` failures so a duplicate profile cannot be reported as newly created.
- Normalize public-key hex before duplicate checking.
- Prevent descriptor wallets from reaching every `importmulti` call path. Show an explanation that descriptor-based tracking requires a future `importdescriptors` implementation.

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
3. Verify that a destination equal to the source multisig address is rejected.
4. Verify that one signature cannot broadcast and two valid signatures can finalize.
5. Verify that cancelling the final confirmation does not place a transaction in the mempool.
6. Verify that the final broadcast shows a transaction ID only after successful broadcast.
7. Verify that invalid address, redeem-script mismatch, invalid M-of-N, and wrong-network profiles cannot spend.
8. Verify that descriptor wallets do not call `importmulti` from any multisig GUI path.
9. Verify duplicate profile and mixed-case duplicate public-key handling.

## Release Criteria

The multisig GUI is ready for a mainnet wallet release only when:

- Every broadcast requires a final confirmation.
- Explicitly invalid profiles cannot create or sign a spend PSBT.
- Self-send cannot be misrepresented as change.
- Unconfirmed UTXOs are not selected by default.
- Descriptor wallet behavior is consistent and does not invoke unsupported legacy import RPCs.
- The Qt build, targeted tests, and the manual 2-of-3 workflow pass.
