DeFCoN Core
===========

DeFCoN Core is the reference implementation of the DeFCoN (DFCN) network: a full node, a wallet and the
masternode software. You can [download DeFCoN Core](https://github.com/defcon-project/defcon/releases/latest)
or [build it yourself](#building) using the guides below.

Running
---------------------
The following are some helpful notes on how to run DeFCoN Core on your native platform.

### Unix

Unpack the files into a directory and run:

- `defcon-qt` (GUI) or
- `defcond` (headless)

### Windows

Unpack the files into a directory, and then run `defcon-qt.exe`.

### macOS

Unpack the archive and run `defcon-qt`. The macOS binaries are not notarized, so macOS asks for
confirmation on first start.

### Need Help?

* See the [DeFCoN website](https://www.dfcn.io) for help and more information.
* Ask for help on the [DeFCoN Discord](https://discord.gg/UpUq7V7Fg7).

Building
---------------------
The following are developer notes on how to build DeFCoN Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)
- [FreeBSD Build Notes](build-freebsd.md)

Development
---------------------
The repository's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [Assumeutxo design](assumeutxo.md)
- [Configuration File](dash-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [InstantSend](instantsend.md)
- [Managing Wallets](managing-wallets.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Transaction Relay Policy](policy/README.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
