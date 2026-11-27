NetBSD Build Guide
======================
**Updated for NetBSD [8.0](https://www.netbsd.org/releases/formal-8/NetBSD-8.0.html)**

This guide describes how to build defcond and command-line utilities on NetBSD.

This guide does not contain instructions for building the GUI.

**This guide has not been tested for building DeFCoN Core and should be treated as an upstream porting reference. Please report your results; contributions welcome.**

Preparation
-------------

You will need the following build dependencies, available through pkgsrc or pkgin:

```
autoconf
automake
boost
git
gmake
gmp
libevent
libtool
pkg-config
Python 3.9 or later (tests)
```

Clone the repository and enter the source tree:

```sh
git clone https://github.com/defcon-project/defcon.git
cd defcon
```

See [dependencies.md](dependencies.md) for a complete overview.

### Building BerkeleyDB

Berkeley DB 4.8 is needed for release-compatible legacy wallets. Descriptor wallets use SQLite. For a node without wallet support, use `--disable-wallet`. Build Berkeley DB with the [included script](../contrib/install_db4.sh) if needed:

```bash
./contrib/install_db4.sh `pwd`
```

from the root of the repository. Then set `BDB_PREFIX` for the next section:

```bash
export BDB_PREFIX="$PWD/db4"
```

### Building DeFCoN Core

**Important**: Use `gmake` (the non-GNU `make` will exit with an error).

With wallet:
```bash
./autogen.sh
./configure --with-gui=no CPPFLAGS="-I/usr/pkg/include" \
    LDFLAGS="-L/usr/pkg/lib" \
    BOOST_CPPFLAGS="-I/usr/pkg/include" \
    BOOST_LDFLAGS="-L/usr/pkg/lib" \
    BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" \
    BDB_CFLAGS="-I${BDB_PREFIX}/include" \
    MAKE=gmake
```

Without wallet:
```bash
./autogen.sh
./configure --with-gui=no --disable-wallet \
    CPPFLAGS="-I/usr/pkg/include" \
    LDFLAGS="-L/usr/pkg/lib" \
    BOOST_CPPFLAGS="-I/usr/pkg/include" \
    BOOST_LDFLAGS="-L/usr/pkg/lib" \
    MAKE=gmake
```

Build and run the tests:
```bash
gmake # use "-j N" here for N parallel jobs
gmake check
```
