# DeFCoN Core

[![CI status](https://github.com/defcon-project/defcon/actions/workflows/build.yml/badge.svg?branch=v22.1.x)](https://github.com/defcon-project/defcon/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/defcon-project/defcon)](https://github.com/defcon-project/defcon/releases/latest)
[![MIT license](https://img.shields.io/badge/license-MIT-blue.svg)](COPYING)

DeFCoN Core is the full node and desktop wallet for the DeFCoN (DFCN) network. Use it to send and receive DFCN, stake from your wallet, or run a masternode. It is based on Dash Core and Bitcoin Core.

**[Download the latest release](https://github.com/defcon-project/defcon/releases/latest)** · [Documentation](doc/README.md) · [Website](https://www.dfcn.io) · [Discord](https://discord.gg/UpUq7V7Fg7) · [X](https://x.com/dfcn_io)

The [official release notes](https://github.com/defcon-project/defcon/releases) cover node, masternode and CoinJoin changes. Nodes still running 22.x stopped receiving the mainnet chain at block 144,768; upgrade them as described in the [v23.0.0 release notes](doc/release-notes.md#upgrading-downgrading-and-going-back).

## Run a wallet or node

Download the archive for your platform and compare its SHA-256 hash with the entry in `SHA256SUMS` on the same release page. Then extract the archive.

| Platform | Archive | Desktop wallet |
| --- | --- | --- |
| Windows (x86-64) | `defcon-windows.zip` | `defcon-windows/defcon-qt.exe` |
| Linux (x86-64, glibc 2.31 or newer) | `defcon-linux.tgz` | `defcon-linux/defcon-qt` |
| macOS 11 or later (Intel) | `defcon-apple.tgz` | `defcon-apple/defcon-qt` |

Open the desktop wallet from the extracted folder. The macOS binaries are unsigned and unnotarized.

To run a node without a graphical interface, start `defcond` from the same folder. Use `defcon-cli getblockchaininfo` to check sync progress.

See the [configuration guide](doc/defcon-conf.md) for `defcon.conf` settings.

## Network

| Mainnet parameter | Value |
| --- | --- |
| Consensus | Proof of stake |
| Target block time | 2.5 minutes |
| Staking reward | 500 DFCN per block |
| Masternode collateral | 1,000,000 DFCN |
| Masternode reward | 10,000 DFCN per block; 5,000 DFCN from the v23.1.0 activation height |
| Transaction locking | InstantSend |
| Block finality | ChainLocks |
| Default peer port | 8192 (TCP) |

## Build and test

For a release build, check out the tag shown on the [releases page](https://github.com/defcon-project/defcon/releases). Development takes place on `v22.1.x`.

[Unix build guide](doc/build-unix.md) · [Windows build guide](doc/build-windows.md) · [macOS build guide](doc/build-osx.md)

After building with wallet support, run the unit and functional tests from the build root.

```sh
make check
test/functional/test_runner.py
```

## Contributing

Start with the [contribution guide](CONTRIBUTING.md). Report security issues privately using the contact in the [security policy](SECURITY.md).

DeFCoN Core is distributed under the [MIT license](COPYING).
