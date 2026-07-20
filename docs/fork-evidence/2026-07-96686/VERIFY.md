# Verification

Run these commands from this directory with OpenSSH 8.2 or newer.

## 1. Verify the Ed25519 signature

```sh
ssh-keygen -Y verify \
  -f ALLOWED_SIGNERS \
  -I defcon-fork-evidence \
  -n defcon-fork-evidence \
  -s SHA256SUMS.sig < SHA256SUMS
```

Expected result:

```text
Good "defcon-fork-evidence" signature for defcon-fork-evidence
```

## 2. Inspect the raw fork structure

```sh
python3 verify_fork_structure.py
```

Expected result:

```text
Verified 4 raw block headers; both height-96686 blocks share parent 96685.
```

## 3. Verify every payload hash

```sh
sha256sum --check SHA256SUMS
```

Every listed file must report `OK`.

## 4. Inspect the signing key

```sh
ssh-keygen -lf PUBLIC_KEY.pub
cat SIGNING_KEY_FINGERPRINT.txt
```

The fingerprints must match. Verification establishes package integrity and
publisher-key continuity; it does not replace independent blockchain analysis.
