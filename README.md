# DeFCoN Core

[![Build](https://github.com/defcon-project/defcon/actions/workflows/build.yml/badge.svg?branch=v22.1.x)](https://github.com/defcon-project/defcon/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/defcon-project/defcon)](https://github.com/defcon-project/defcon/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](COPYING)

DeFCoN Core is the reference implementation of the **DeFCoN (DFCN)** network: a full node, a wallet and
the masternode software, in one program. It is derived from Dash Core, and through it from Bitcoin Core.

- **Website:** <https://www.dfcn.io>
- **Downloads:** [latest release](https://github.com/defcon-project/defcon/releases/latest)
- **Documentation:** [`doc/`](doc/README.md)
- **Community:** [Discord](https://discord.gg/UpUq7V7Fg7) · [X / Twitter](https://x.com/dfcn_io)

## The network

| | |
|---|---|
| Consensus | Proof-of-stake (the first 999 blocks were proof-of-work) |
| Block target | 2.5 minutes |
| Masternode collateral | 1,000,000 DFCN |
| Finality | LLMQ ChainLocks and InstantSend |
| Default P2P port | 8192 |

Version 23 activates at block **144,888** on mainnet and is a mandatory upgrade: every node must run it
before block **144,768**, where v22 nodes stop receiving the chain. It moves ChainLocks to the Q60
quorum (60 members, 41 of whom must sign), where two ChainLocks for the same height would need at
least 22 members to sign against their own software, and starts the Sentinel Layer, a service-level
liveness record for masternodes, which starts observing at block **145,464** (576 blocks after the
activation) and does not punish anyone in v23. See the
[v23.0.0 release](https://github.com/defcon-project/defcon/releases/tag/v23.0.0) for the full notes.

## Running a node

Download the archive for your platform from the
[latest release](https://github.com/defcon-project/defcon/releases/latest), verify it against the
published `SHA256SUMS`, and start `defcond` (daemon) or `defcon-qt` (graphical wallet).
`defcon-cli help` lists the RPC commands.

| Platform | Archive | Notes |
|---|---|---|
| Linux, x86-64 | `defcon-linux.tgz` | Built for glibc 2.31 (Ubuntu 20.04) and newer |
| Windows, x86-64 | `defcon-windows.zip` | Unpack and run; there is no installer |
| macOS 11 or newer, Intel | `defcon-apple.tgz` | Cross-built, unsigned and not notarized; no Apple Silicon build |

Settings go in `defcon.conf`, in the data directory: `~/.defcon/` on Linux, `%APPDATA%\Defcon\` on
Windows and `~/Library/Application Support/Defcon/` on macOS. See
[`doc/defcon-conf.md`](doc/defcon-conf.md) and the other guides in [`doc/`](doc/README.md).

## Releases and branches

Nodes should run a release. Releases are tagged `vX.Y.Z` and published on the
[releases page](https://github.com/defcon-project/defcon/releases) with their notes, archives and
checksums. The default branch, `v22.1.x`, is the development branch: despite its name it already
carries the work towards the next release, and it is not a release itself.

## Building from source

The simplest route is the depends system, which builds every dependency at a pinned version. The
`git checkout` line builds the latest release; leave it out to build the development branch:

```bash
sudo apt install -y build-essential automake autotools-dev cmake pkg-config python3 bison git make libtool
git clone https://github.com/defcon-project/defcon
cd defcon
git checkout v23.0.0
make -C depends HOST=x86_64-pc-linux-gnu -j4
./autogen.sh
CONFIG_SITE=$PWD/depends/x86_64-pc-linux-gnu/share/config.site ./configure
make -j4
```

Platform-specific notes are in [`doc/`](doc/README.md#building): [Unix](doc/build-unix.md),
[macOS](doc/build-osx.md), [Windows](doc/build-windows.md).

## Testing

```bash
make check                          # unit tests
test/functional/test_runner.py      # functional tests (needs a build with the wallet)
```

Every pull request is built and tested by [GitHub Actions](.github/workflows/build.yml) before it can
be merged.

## Contributing and security

Contributions are welcome; see [`CONTRIBUTING.md`](CONTRIBUTING.md). Please report security issues
privately as described in [`SECURITY.md`](SECURITY.md), never in a public issue.

## License

DeFCoN Core is released under the MIT license. See [`COPYING`](COPYING).
