# macOS application packaging

The `macdeployqtplus` script should not be run manually. Instead, after building as usual:

```bash
make deploy
```

The deploy target packages `Defcon-Qt.app` as a ZIP. The planned v23.1.0 release package is `defcon-apple.tgz`, containing the unsigned Intel binaries. See the [release checklist](../../doc/release-process.md).

## SDK Extraction

### Step 1: Obtaining `Xcode.app`

A free Apple Developer Account is required to proceed.

Our macOS SDK can be extracted from
[Xcode_15.xip](https://download.developer.apple.com/Developer_Tools/Xcode_15/Xcode_15.xip).

Alternatively, after logging in to your account go to 'Downloads', then 'More'
and search for [`Xcode 15`](https://developer.apple.com/download/all/?q=Xcode%2015).

An Apple ID and cookies enabled for the hostname are needed to download this.

The `sha256sum` of the downloaded XIP archive should be `4daaed2ef2253c9661779fa40bfff50655dc7ec45801aba5a39653e7bcdde48e`.

To extract the `.xip` on Linux:

```bash
# Install/clone tools needed for extracting Xcode.app
apt install cpio
git clone https://github.com/bitcoin-core/apple-sdk-tools.git

# Unpack the .xip and place the resulting Xcode.app in your current
# working directory
python3 apple-sdk-tools/extract_xcode.py -f Xcode_15.xip | cpio -d -i
```

On macOS:

```bash
xip -x Xcode_15.xip
```

### Step 2: Generating the SDK tarball from `Xcode.app`

To generate the SDK, run the script [`gen-sdk`](./gen-sdk) with the
path to `Xcode.app` (extracted in the previous stage) as the first argument.

```bash
./contrib/macdeploy/gen-sdk '/path/to/Xcode.app'
```

The generated archive should be: `Xcode-15.0-15A240d-extracted-SDK-with-libcxx-headers.tar.gz`.
The `sha256sum` should be `c0c2e7bb92c1fee0c4e9f3a485e4530786732d6c6dd9e9f418c282aa6892f55d`.

The Guix container's [`setup-sdk`](../containers/guix/scripts/setup-sdk) refuses an archive with
any other hash, so change the hash there too when the SDK changes. It also refuses an SDK directory
that it did not extract itself; remove that directory to extract a verified copy.

## Deterministic macOS App Notes

The [Guix build tools](../guix/README.md) retain the upstream detached-signature workflow for experimental builds. DeFCoN release builds use the [depends system](../../depends/README.md). The public macOS package contains individual command-line and Qt executables. Compare the download with the archive checksum on its [official release page](https://github.com/defcon-project/defcon/releases).
