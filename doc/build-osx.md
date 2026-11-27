# macOS Build Guide

These instructions cover native Intel macOS builds of `defcond`, the command-line tools and `defcon-qt`. For the published macOS archive, see the [official releases](https://github.com/defcon-project/defcon/releases).

## Dependencies

The following dependencies are **required**:

Library                                                    | Purpose    | Description
-----------------------------------------------------------|------------|----------------------
[automake](https://formulae.brew.sh/formula/automake)      | Build      | Generate makefile
[libtool](https://formulae.brew.sh/formula/libtool)        | Build      | Shared library support
[pkg-config](https://formulae.brew.sh/formula/pkg-config)  | Build      | Configure compiler and linker flags
[gmp](https://formulae.brew.sh/formula/gmp)                 | Arithmetic | Integer arithmetic required by configure
[boost](https://formulae.brew.sh/formula/boost)            | Utility    | Library for threading, data structures, etc
[libevent](https://formulae.brew.sh/formula/libevent)      | Networking | OS independent asynchronous networking

The following dependencies are **optional**:

Library                                                         | Purpose          | Description
--------------------------------------------------------------- |------------------|----------------------
[qt@5](https://formulae.brew.sh/formula/qt@5)                   | GUI              | GUI toolkit (only needed when GUI enabled)
[qrencode](https://formulae.brew.sh/formula/qrencode)           | QR codes in GUI  | Generating QR codes (only needed when GUI enabled)
[zeromq](https://formulae.brew.sh/formula/zeromq)               | ZMQ notification | Allows generating ZMQ notifications (requires ZMQ version >= 4.0.0)
[sqlite](https://formulae.brew.sh/formula/sqlite)               | SQLite DB        | Wallet storage (only needed when wallet enabled)
[miniupnpc](https://formulae.brew.sh/formula/miniupnpc)         | UPnP Support     | Firewall-jumping support (needed for port mapping support)
[libnatpmp](https://formulae.brew.sh/formula/libnatpmp)         | NAT-PMP Support  | Firewall-jumping support (needed for port mapping support)
[python3](https://formulae.brew.sh/formula/python@3.9)          | Testing          | Python Interpreter (only needed when running the test suite)

See [dependencies.md](dependencies.md) for a complete overview.

## Preparation

The commands in this guide should be executed in a Terminal application.
macOS comes with a built-in Terminal located in:

```
/Applications/Utilities/Terminal.app
```

### 1. Xcode Command Line Tools

The Xcode Command Line Tools are a collection of build tools for macOS.
These tools must be installed in order to build DeFCoN Core from source.

To install, run the following command from your terminal:

``` bash
xcode-select --install
```

Upon running the command, you should see a popup appear.
Click on `Install` to continue the installation process.

### 2. Homebrew Package Manager

Homebrew is a package manager for macOS that allows one to install packages from the command line easily.
While several package managers are available for macOS, this guide will focus on Homebrew as it is the most popular.
Since the examples in this guide which walk through the installation of a package will use Homebrew, it is recommended that you install it to follow along.
Otherwise, you can adapt the commands to your package manager of choice.

To install the Homebrew package manager, see: https://brew.sh

Note: If you run into issues while installing Homebrew or pulling packages, refer to [Homebrew's troubleshooting page](https://docs.brew.sh/Troubleshooting).

### 3. Install Required Dependencies

The first step is to download the required dependencies.
These dependencies represent the packages required to get a barebones installation up and running.
To install, run the following from your terminal:

``` bash
brew install automake libtool boost gmp pkg-config libevent
```

### 4. Clone DeFCoN Core repository

`git` should already be installed by default on your system.
Now that all the required dependencies are installed, let's clone the DeFCoN Core repository to a directory.
All build scripts and commands will run from this directory.

``` bash
git clone https://github.com/defcon-project/defcon.git
cd defcon
```

### 5. Install Optional Dependencies

#### Wallet Dependencies

It is not necessary to build wallet functionality to run `defcond` or  `defcon-qt`.
Descriptor wallets use SQLite. Legacy wallets compatible with the published binaries use Berkeley DB 4.8. The [included installer](../contrib/install_db4.sh) builds it in a prefix you choose and prints the flags to use with `./configure`.

###### Descriptor Wallet Support

Note: Apple has included a useable `sqlite` package since macOS 10.14.
You may not need to install this package.

`sqlite` is required to enable support for descriptor wallets.
Skip if you don't intend to use descriptor wallets.

``` bash
brew install sqlite
```
---

#### GUI Dependencies

###### Qt

DeFCoN Core includes a GUI built with the cross-platform Qt Framework.
To compile the GUI, we need to install `qt@5`.
Skip if you don't intend to use the GUI.

``` bash
brew install qt@5
```

If multiple major versions of Qt are installed, select the Qt 5 tools and libraries when configuring this build.
See the notes in [bitcoin#7714](https://github.com/bitcoin/bitcoin/issues/7714).

###### qrencode

The GUI can encode addresses in a QR Code. To build in QR support for the GUI, install `qrencode`.
Skip if not using the GUI or don't want QR code functionality.

``` bash
brew install qrencode
```
---

#### Port Mapping Dependencies

###### miniupnpc

miniupnpc may be used for UPnP port mapping.
Skip if you do not need this functionality.

``` bash
brew install miniupnpc
```

###### libnatpmp

libnatpmp may be used for NAT-PMP port mapping.
Skip if you do not need this functionality.

``` bash
brew install libnatpmp
```

Note: UPnP and NAT-PMP support will be compiled in and disabled by default.
Check out the [further configuration](#further-configuration) section for more information.

---

#### ZMQ Dependencies

Support for ZMQ notifications requires the following dependency.
Skip if you do not need ZMQ functionality.

``` bash
brew install zeromq
```

ZMQ is automatically compiled in and enabled if the dependency is detected.
Check out the [further configuration](#further-configuration) section for more information.

For more information on ZMQ, see: [zmq.md](zmq.md)

---

#### Test Suite Dependencies

There is an included test suite that is useful for testing code changes when developing.
To run the test suite (recommended), you will need to have Python 3 installed:

``` bash
brew install python
```

---

#### Deploy Dependencies

You can deploy a `.zip` containing the DeFCoN Core application using `make deploy`.
It is required that you have `python` installed.

## Building DeFCoN Core

### 1. Configuration

There are many ways to configure DeFCoN Core, here are a few common examples:

##### Wallet (BDB + SQlite) Support, No GUI:

If Berkeley DB 4.8 is available to configure, then legacy wallet support will be built.
If `sqlite` is installed, then descriptor wallet support will also be built.
Additionally, this explicitly disables the GUI.

``` bash
./autogen.sh
./configure --with-gui=no
```

###### Berkeley DB

It is recommended to use Berkeley DB 4.8. If you have to build it yourself,
you can use [the installation script included in contrib/](../contrib/install_db4.sh)
like so:

```shell
./contrib/install_db4.sh .
```

##### Wallet (only SQlite) and GUI Support:

This explicitly enables the GUI and disables legacy wallet support.
If Qt 5 is missing, this will throw an error.
If `sqlite` is installed then descriptor wallet functionality will be built.
If `sqlite` is not installed, then wallet functionality will be disabled.

``` bash
./autogen.sh
./configure --without-bdb --with-gui=yes
```

##### No Wallet or GUI

``` bash
./autogen.sh
./configure --disable-wallet --with-gui=no
```

##### Further Configuration

You may want to dig deeper into the configuration options to achieve your desired behavior.
Examine the output of the following command for a full list of configuration options:

``` bash
./configure --help
```

### 2. Compile

After configuration, you are ready to compile.
Run the following in your terminal to compile DeFCoN Core:

``` bash
make        # use "-j N" here for N parallel jobs
make check  # Run tests if Python 3 is available
```

### 3. Deploy (optional)

To create `DeFCoN.zip` containing `Defcon-Qt.app`, run the following command:

``` bash
make deploy
```

## Running DeFCoN Core

DeFCoN Core should now be available at `./src/defcond`.
If you compiled support for the GUI, it should be available at `./src/qt/defcon-qt`.

The first time you run `defcond` or `defcon-qt`, it will start downloading the blockchain.
This process could take many hours, or even days on slower than average systems.

By default, blockchain and wallet data files will be stored in:

``` bash
/Users/${USER}/Library/Application Support/Defcon/
```

Before running, you may create an empty configuration file:

```shell
mkdir -p "/Users/${USER}/Library/Application Support/Defcon"

touch "/Users/${USER}/Library/Application Support/Defcon/defcon.conf"

chmod 600 "/Users/${USER}/Library/Application Support/Defcon/defcon.conf"
```

You can monitor the download process by looking at the debug.log file:

```shell
tail -f $HOME/Library/Application\ Support/Defcon/debug.log
```

## Other commands:

```shell
./src/defcond -daemon      # Starts the defcond daemon.
./src/defcon-cli --help    # Outputs a list of command-line options.
./src/defcon-cli help      # Outputs a list of RPC commands when the daemon is running.
./src/qt/defcon-qt -server # Starts the defcon-qt server mode, allows defcon-cli control
```
