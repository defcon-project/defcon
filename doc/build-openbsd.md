OpenBSD build guide
======================
(updated for OpenBSD 6.7)

This guide describes how to build defcond, defcon-qt, and command-line utilities on OpenBSD.

**This guide is inherited from upstream and has not been tested for DeFCoN Core. Please report your results; contributions welcome.**

Preparation
-------------

Run the following as root to install the base dependencies for building:

```bash
pkg_add git gmake libevent libtool
pkg_add qt5 # (optional for enabling the GUI)
pkg_add autoconf # (select highest version, e.g. 2.69)
pkg_add automake # (select highest version, e.g. 1.15)
pkg_add python # (select highest version, e.g. 3.9)
pkg_add gmp
pkg_add bash
pkg_add boost
```

From a directory owned by your normal user:

```sh
git clone https://github.com/defcon-project/defcon.git
cd defcon
```

See [dependencies.md](dependencies.md) for a complete overview.

Select a compiler and C++ standard library with C++17 support. The examples use the same toolchain for configuration and compilation.

### Building BerkeleyDB

Berkeley DB 4.8 is needed for release-compatible legacy wallets. Descriptor wallets use SQLite. For a node without wallet support, use `--disable-wallet`. Build Berkeley DB with the [included script](../contrib/install_db4.sh) if needed:

```bash
./contrib/install_db4.sh `pwd` CC=cc CXX=c++
```

from the root of the repository. Then set `BDB_PREFIX` for the next section:

```bash
export BDB_PREFIX="$PWD/db4"
```

### Building DeFCoN Core

**Important**: Use `gmake` (the non-GNU `make` will exit with an error).

Preparation:
```bash
export AUTOCONF_VERSION=2.69 # replace this with the autoconf version that you installed
export AUTOMAKE_VERSION=1.15 # replace this with the automake version that you installed
./autogen.sh
```
Make sure `BDB_PREFIX` is set to the appropriate path from the above steps.

To configure with wallet:
```bash
./configure --with-gui=no CC=cc CXX=c++ \
    BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" \
    BDB_CFLAGS="-I${BDB_PREFIX}/include" \
    MAKE=gmake
```

To configure without wallet:
```bash
./configure --disable-wallet --with-gui=no CC=cc CC_FOR_BUILD=cc CXX=c++ MAKE=gmake
```

To configure with GUI:
```bash
./configure --with-gui=yes CC=cc CXX=c++ \
    BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" \
    BDB_CFLAGS="-I${BDB_PREFIX}/include" \
    MAKE=gmake
```

Build and run the tests:
```bash
gmake # use "-j N" here for N parallel jobs
gmake check
```

Resource limits
-------------------

If the build runs into out-of-memory errors, the instructions in this section
might help.

The standard ulimit restrictions in OpenBSD are very strict:

    data(kbytes)         1572864

This, unfortunately, in some cases not enough to compile some `.cpp` files in the project,
(see issue [#6658](https://github.com/bitcoin/bitcoin/issues/6658)).
If your user is in the `staff` group the limit can be raised with:

    ulimit -d 3000000

The change will only affect the current shell and processes spawned by it. To
make the change system-wide, change `datasize-cur` and `datasize-max` in
`/etc/login.conf`, and reboot.
