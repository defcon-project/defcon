# Build DeFCoN Core dependencies

The depends system builds pinned dependency versions for a selected host and writes a `config.site` file for Core's configure step. Run the following from the repository root on an x86-64 Linux build machine with the [Unix build prerequisites](../doc/build-unix.md) and `curl` installed (the depends build downloads its sources with `curl`):

```sh
make -C depends HOST=x86_64-pc-linux-gnu -j4
./autogen.sh
CONFIG_SITE="$PWD/depends/x86_64-pc-linux-gnu/share/config.site" ./configure
make -j4
```

## Release targets

| Host triplet | Target | Build notes |
| --- | --- | --- |
| `x86_64-pc-linux-gnu` | Linux x86-64 | [Unix](../doc/build-unix.md) |
| `x86_64-w64-mingw32` | Windows x86-64 | [Windows cross-compilation](../doc/build-windows.md) |
| `x86_64-apple-darwin` | Intel macOS | [macOS](../doc/build-osx.md) and [SDK extraction](../contrib/macdeploy/README.md#sdk-extraction) |

For a cross-build, use the same triplet for `HOST` and the path to `config.site`. The macOS cross-build needs Clang 18 or later, LLD and the LLVM tools (on Debian or Ubuntu: `apt install clang lld llvm g++ zip`) and the extracted macOS SDK in `depends/SDKs`, or in the directory set with `SDK_PATH`. The [host recipes](hosts/) include additional toolchains. The [release checklist](../doc/release-process.md) specifies which platforms receive project binaries.

## Selected build options

Pass options to the depends make invocation, for example `make -C depends HOST=x86_64-pc-linux-gnu NO_QT=1`.

| Option | Effect |
| --- | --- |
| `SOURCES_PATH`, `BASE_CACHE` | Set the locations of source archives and built-package caches. |
| `SDK_PATH` | Set the parent directory of the extracted macOS SDK. |
| `NO_QT=1`, `NO_QR=1` | Omit Qt or the QR encoder and related dependencies. |
| `NO_WALLET=1` | Omit wallet libraries and set the wallet-disabled configure default. |
| `NO_BDB=1`, `NO_SQLITE=1` | Omit Berkeley DB or SQLite. |
| `NO_ZMQ=1`, `NO_UPNP=1`, `NO_NATPMP=1`, `NO_USDT=1` | Omit the corresponding optional dependencies. |
| `MULTIPROCESS=1` | Build Cap'n Proto and libmultiprocess for the [multiprocess](../doc/multiprocess.md) binaries. |
| `LOG=1` | Keep a build log for each package. |

`make -C depends download` fetches source archives. The platform-specific targets are `download-linux`, `download-win` and `download-osx`. See the [Makefile](Makefile) for the full option list, [build and cache design](description.md) for the dependency model, and [package recipes](packages.md) for adding a library.
