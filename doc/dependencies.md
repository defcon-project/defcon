Dependencies
============

Pinned versions are taken from the [depends recipes](../depends/packages/). GMP headers and the GMP library are required by configure. You can find instructions for installing them in the `build-*.md` file for your platform.

| Dependency | Version used | Minimum required | Shared | [Bundled Qt library](https://doc.qt.io/qt-5/configure-options.html#third-party-libraries) |
| --- | --- | --- | --- | --- |
| Berkeley DB | [4.8.30](https://www.oracle.com/technetwork/database/database-technologies/berkeleydb/downloads/index.html) | 4.8.x |  |  |
| Boost | [1.81.0](https://www.boost.org/users/download/) | [1.73.0](https://github.com/bitcoin/bitcoin/pull/29066) |  |  |
| Clang<sup>[ \* ](#note1)</sup> |  | [5.0+](https://releases.llvm.org/download.html) (C++17 support, with a standard library that provides `std::filesystem`) |  |  |
| fontconfig | [2.12.1](https://www.freedesktop.org/software/fontconfig/release/) |  | Yes |  |
| FreeType | [2.11.0](https://download.savannah.gnu.org/releases/freetype) |  |  | [Yes](../depends/packages/qt.mk) (Android only) |
| GCC |  | [8+](https://gcc.gnu.org/) (C++17 and `std::filesystem`) |  |  |
| glibc | | [2.31](https://www.gnu.org/software/libc/) |  |  |
| GMP | [6.3.0](../depends/packages/gmp.mk) |  |  |  |
| HarfBuzz-NG |  |  |  | [Yes](../depends/packages/qt.mk) |
| libevent | [2.1.12-stable](https://github.com/libevent/libevent/releases) | [2.0.21](https://github.com/bitcoin/bitcoin/pull/18676) |  |  |
| libnatpmp | git commit [07004b9...](https://github.com/miniupnp/libnatpmp/tree/07004b97cf691774efebe70404cf22201e4d330d) |  |  |  |
| libpng |  |  |  | [Yes](../depends/packages/qt.mk) |
| Linux Kernel | [N/A](https://www.kernel.org/) | 3.2.0 | | |
| MiniUPnPc | [2.2.2](https://miniupnp.tuxfamily.org/files) |  |  |  |
| PCRE |  |  |  | [Yes](../depends/packages/qt.mk) |
| Python (tests) |  | [3.9](https://www.python.org/downloads) |  |  |
| qrencode | [4.1.1](https://fukuchi.org/works/qrencode) |  |  |  |
| Qt | [5.15.19](https://download.qt.io/archive/qt/5.15/) | 5.15.0 | |  |
| SQLite | [3.38.5](https://sqlite.org/download.html) | [3.7.17](https://github.com/bitcoin/bitcoin/pull/19077) |  |  |
| XCB | [1.14](../depends/packages/libxcb.mk) |  | Yes (Linux only) |  |
| systemtap ([tracing](tracing.md))| [4.8](https://sourceware.org/ftp/systemtap/releases/) |  |  | |
| xkbcommon | [0.8.4](../depends/packages/libxkbcommon.mk) |  | Yes (Linux only) |  |
| ZeroMQ | [4.3.5](https://github.com/zeromq/libzmq/releases) | 4.0.0 |  |  |
| zlib |  |  |  | [Yes](../depends/packages/qt.mk) |

<a name="note1">Note \*</a> : When compiling with `-stdlib=libc++`, the minimum supported libc++ version is 7.0.

Controlling dependencies
------------------------
Some dependencies are not needed in all configurations. The following are some factors that affect the dependency list.

#### Options passed to `./configure`
* MiniUPnPc is not needed with `--without-miniupnpc`.
* Berkeley DB is not needed with `--disable-wallet` or `--without-bdb`.
* SQLite is not needed with `--disable-wallet` or `--without-sqlite`.
* libnatpmp is not needed with `--without-natpmp`.
* Qt is not needed with `--without-gui`.
* If the qrencode dependency is absent, QR support won't be added. To force an error when that happens, pass `--with-qrencode`.
* If the systemtap dependency is absent, USDT probes are omitted.
* ZeroMQ notifications require libzmq 4 or later. Configure detects it automatically unless `--disable-zmq` is set.
