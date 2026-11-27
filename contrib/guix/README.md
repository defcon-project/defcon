# Guix build tooling

This directory contains build and attestation scripts inherited from Dash Core. The [DeFCoN release checklist](../../doc/release-process.md) calls for building the tagged source with depends and publishing a checksum for each archive.

## Building

For work on the Guix port, start with the [installation notes](INSTALL.md), the [manifest](manifest.scm) and the [build driver](guix-build). The [shared configuration](libexec/prelude.bash) still defaults to `dashcore-` for distribution names. `HOSTS` selects target triples and `JOBS` sets build parallelism. `SOURCES_PATH` and `BASE_CACHE` select reusable dependency directories. Darwin targets also require the extracted SDK under `SDK_PATH`. The release checklist lists the targets which receive project binaries.

## Attesting to build outputs

[guix-attest](guix-attest) records a builder's checksums in a builder-supplied attestation repository.

## Verifying build output attestations

[guix-verify](guix-verify) verifies the builder attestations available in that repository. For published DeFCoN archives, compare the archive with `SHA256SUMS` from the same [official release](https://github.com/defcon-project/defcon/releases).

## Codesigning build outputs

[guix-codesign](guix-codesign) uses a detached-signature repository. The upstream Dash signatures apply to Dash artifacts. The DeFCoN macOS archive is distributed unsigned.
