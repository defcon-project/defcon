WINDOWS BUILD NOTES
====================

Below are some notes on how to build DeFCoN Core for Windows.

The options known to work for building DeFCoN Core on Windows are:

* On Linux, using the [Mingw-w64](https://www.mingw-w64.org/) cross compiler tool chain.
* On Windows, using [Windows Subsystem for Linux (WSL)](https://learn.microsoft.com/windows/wsl/about) and Mingw-w64.

Other options which may work, but which have not been extensively tested are (please contribute instructions):

* On Windows, using a POSIX compatibility layer application such as [cygwin](https://www.cygwin.com/) or [msys2](https://www.msys2.org/).

The instructions below work on Ubuntu and Debian. Make sure the distribution's `g++-mingw-w64-x86-64-posix`
package meets the minimum required `g++` version specified in [dependencies.md](dependencies.md).

Installing Windows Subsystem for Linux
---------------------------------------

Follow the upstream installation instructions, available [here](https://learn.microsoft.com/windows/wsl/install).

Cross-compilation
-------------------

The steps below can be performed on Ubuntu or WSL. The depends system
will also work on other Linux distributions, however the commands for
installing the toolchain will be different.

First, install the general dependencies:

    sudo apt-get install build-essential libtool autotools-dev automake pkg-config bsdmainutils cmake curl git bison python3 xz-utils

A host toolchain (`build-essential`) is necessary because some dependency
packages need to build host utilities that are used in the build process.

See [dependencies.md](dependencies.md) for a complete overview.

## Building for 64-bit Windows

The first step is to install the mingw-w64 cross-compilation tool chain:

    sudo apt-get install g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev

Once the toolchain is installed the build steps are common:

With WSL, build from the Linux filesystem, for example `~/src/defcon`. Run the commands below from the repository root. On a Linux host, only the build commands are needed. WSL can launch Windows executables during Autoconf checks. To prevent that, disable Windows binary interop for the build with `sudo bash -c "echo 0 > /proc/sys/fs/binfmt_misc/status"` and enable it again afterwards with `sudo bash -c "echo 1 > /proc/sys/fs/binfmt_misc/status"`.

    cd depends
    make HOST=x86_64-w64-mingw32
    cd ..
    ./autogen.sh
    CONFIG_SITE=$PWD/depends/x86_64-w64-mingw32/share/config.site ./configure --prefix=/
    make # use "-j N" for N parallel jobs

## Depends system

For further documentation on the depends system see [README.md](../depends/README.md) in the depends directory.

Installation
-------------

After building using the Windows subsystem it can be useful to copy the compiled
executables to a directory on the Windows drive in the same directory structure
as they appear in the release `.zip` archive. This can be done in the following
way. This will install to `c:\workspace\defcon`, for example:

    make install DESTDIR=/mnt/c/workspace/defcon

The installer target (`make deploy`) is inherited from upstream and is not supported: DeFCoN Core
releases for Windows are `.zip` archives.
