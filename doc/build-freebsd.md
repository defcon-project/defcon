# FreeBSD Build Guide

**Updated for FreeBSD [12.2](https://www.freebsd.org/releases/12.2R/announce.html)**

This guide describes how to build defcond, command-line utilities, and GUI on FreeBSD.

**This guide is inherited from upstream and has not been tested for DeFCoN Core. Please report your results; contributions welcome.**

## Dependencies

The following dependencies are **required**:

 Library                                                               | Purpose    | Description
 ----------------------------------------------------------------------|------------|----------------------
 [autoconf](https://cgit.freebsd.org/ports/tree/devel/autoconf/)     | Build      | Automatically configure software source code
 [automake](https://cgit.freebsd.org/ports/tree/devel/automake/)     | Build      | Generate makefile (requires autoconf)
 [libtool](https://cgit.freebsd.org/ports/tree/devel/libtool/)       | Build      | Shared library support
 [pkgconf](https://cgit.freebsd.org/ports/tree/devel/pkgconf/)       | Build      | Configure compiler and linker flags
 [git](https://cgit.freebsd.org/ports/tree/devel/git/)               | Clone      | Version control system
 [gmake](https://cgit.freebsd.org/ports/tree/devel/gmake/)           | Compile    | Generate executables
 [boost-libs](https://cgit.freebsd.org/ports/tree/devel/boost-libs/) | Utility    | Library for threading, data structures, etc
 [libevent](https://cgit.freebsd.org/ports/tree/devel/libevent/)     | Networking | OS independent asynchronous networking


The following dependencies are **optional**:

  Library                                                                    | Purpose          | Description
  ---------------------------------------------------------------------------|------------------|----------------------
  [db5](https://cgit.freebsd.org/ports/tree/databases/db5/)                | Berkeley DB      | Wallet storage (only needed when wallet enabled)
  [qt5](https://cgit.freebsd.org/ports/tree/devel/qt5/)                    | GUI              | GUI toolkit (only needed when GUI enabled)
  [libqrencode](https://cgit.freebsd.org/ports/tree/graphics/libqrencode/) | QR codes in GUI  | Generating QR codes (only needed when GUI enabled)
  [libzmq4](https://cgit.freebsd.org/ports/tree/net/libzmq4/)              | ZMQ notification | Allows generating ZMQ notifications (requires ZMQ version >= 4.0.0)
  [sqlite3](https://cgit.freebsd.org/ports/tree/databases/sqlite3/)        | SQLite DB        | Wallet storage (only needed when wallet enabled)
  [python3](https://cgit.freebsd.org/ports/tree/lang/python3/)             | Testing          | Python Interpreter (only needed when running the test suite)

  See [dependencies.md](dependencies.md) for a complete overview.

GMP is required by the DeFCoN build. Install its headers and library along with the other base dependencies.

## Preparation

### 1. Install Required Dependencies
Run the following as root to install the base dependencies for building.

```bash
pkg install autoconf automake boost-libs git gmake gmp libevent libtool pkgconf
```

### 2. Clone DeFCoN Core Repo
Now that `git` and all the required dependencies are installed, let's clone the DeFCoN Core repository to a directory. All build scripts and commands will run from this directory.
``` bash
git clone https://github.com/defcon-project/defcon.git
cd defcon
```

### 3. Install Optional Dependencies

#### Wallet Dependencies
It is not necessary to build wallet functionality to run defcond or the GUI. To enable legacy wallets, you must install `db5`. To enable [descriptor wallets](descriptors.md), `sqlite3` is required. Skip `db5` if you intend to *exclusively* use descriptor wallets

###### Legacy Wallet Support
`db5` is required to enable support for legacy wallets. Skip if you don't intend to use legacy wallets

```bash
pkg install db5
```

###### Descriptor Wallet Support

`sqlite3` is required to enable support for descriptor wallets. Skip if you don't intend to use descriptor wallets.
``` bash
pkg install sqlite3
```
---

#### GUI Dependencies
###### Qt5

DeFCoN Core includes a GUI built with the cross-platform Qt Framework. To compile the GUI, we need to install `qt5`. Skip if you don't intend to use the GUI.
```bash
pkg install qt5
```
###### libqrencode

The GUI can encode addresses in a QR Code. To build in QR support for the GUI, install `libqrencode`. Skip if not using the GUI or don't want QR code functionality.
```bash
pkg install libqrencode
```
---

#### Test Suite Dependencies
There is an included test suite that is useful for testing code changes when developing.
To run the test suite (recommended), you will need to have Python 3 installed:

```bash
pkg install python3 databases/py-sqlite3
```
---

## Building DeFCoN Core

### 1. Configuration

There are many ways to configure DeFCoN Core, here are a few common examples:
##### Wallet (BDB + SQlite) Support, No GUI:
This explicitly enables legacy wallet support and disables the GUI. If `sqlite3` is installed, then descriptor wallet support will be built.
```bash
./autogen.sh
./configure --with-gui=no --with-incompatible-bdb \
    BDB_LIBS="-ldb_cxx-5" \
    BDB_CFLAGS="-I/usr/local/include/db5" \
    MAKE=gmake
```

##### Wallet (only SQlite) and GUI Support:
This explicitly enables the GUI and disables legacy wallet support. If `qt5` is not installed, this will throw an error. If `sqlite3` is installed then descriptor wallet functionality will be built. If `sqlite3` is not installed, then wallet functionality will be disabled.
```bash
./autogen.sh
./configure --without-bdb --with-gui=yes MAKE=gmake
```
##### No Wallet or GUI
``` bash
./autogen.sh
./configure --disable-wallet --with-gui=no MAKE=gmake
```

### 2. Compile
**Important**: Use `gmake` (the non-GNU `make` will exit with an error).

```bash
gmake # use "-j N" for N parallel jobs
gmake check # Run tests if Python 3 is available
```
