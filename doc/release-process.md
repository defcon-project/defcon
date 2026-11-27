Release Process
====================

## DeFCoN Core releases

Every DeFCoN Core release must satisfy this checklist.

In the release pull request:

* [ ] Set the version in `configure.ac` (`_CLIENT_VERSION_*`, with `_CLIENT_VERSION_IS_RELEASE` set to `true`).
* [ ] Add a recent mainnet checkpoint to `checkpointData` and update the fork-recovery anchor
  (`nForkRecoveryAnchorHeight`, `hashForkRecoveryAnchor`) in `src/chainparams.cpp`, both from current mainnet data.
* [ ] Collect the `doc/release-notes-*.md` fragments into `doc/release-notes.md`.
* [ ] Check the platform table and official download link in `README.md`. Update the supported versions in `SECURITY.md`.

Binaries, built from the tagged commit with the depends system:

* [ ] `defcon-linux.tgz` (x86_64-pc-linux-gnu), `defcon-windows.zip` (x86_64-w64-mingw32) and `defcon-apple.tgz`
  (x86_64-apple-darwin, cross-built and unsigned). Keep these file names: download links and scripts rely on them.
* [ ] Each archive contains `COPYING` next to the binaries (the Windows archive also `doc/README_windows.txt` as `readme.txt`).
* [ ] Regenerate the manual pages from the release binaries with
  [`contrib/devtools/gen-manpages.py`](../contrib/devtools/gen-manpages.py), and commit them.

On the GitHub releases page:

* [ ] The archives; `SHA256SUMS` (one line per archive); `SHA256SUMS.binaries` (the binaries inside the
  archives); `BUILDINFO.txt` (source commit, tree, tag and build targets); and the release notes, with
  the verification commands for each platform.
* [ ] Point the download page on https://www.dfcn.io/wallet/ to the new release.

## Activation and distribution

Record the mainnet activation block from the tagged source in the versioned release notes. Give operators the upgrade deadline in blocks and label any calendar time as an estimate. The root README links to the official releases page.

The project distributes its releases through [defcon-project/defcon](https://github.com/defcon-project/defcon/releases). The [Guix tooling](../contrib/guix/README.md) comes from upstream and is available for reproducible-build research. The published DeFCoN release procedure uses the depends system described above.
