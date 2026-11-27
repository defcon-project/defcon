# DeFCoN Core desktop wallet

The desktop wallet uses Qt Widgets. The [Qt recipe](../../depends/packages/qt.mk) pins the version used by the depends build. For a local build, follow the [platform instructions](../../doc/README.md#building) with Qt and wallet support enabled.

From the build root, start the wallet with `./src/qt/defcon-qt`. A build with Qt tests enabled produces `src/qt/test/test_defcon-qt`.

## Finding your way around

| Task | Source |
| --- | --- |
| Change the main window or the view of a loaded wallet | [Main window](bitcoingui.cpp) · [Wallet view](walletview.cpp) |
| Edit layouts and graphics | [Qt Designer forms](forms/) · [Resources](res/) |
| Change how DFCN amounts are displayed | [Display units](bitcoinunits.cpp) |
| Handle a `defcon:` payment link | [Payment server](paymentserver.cpp) · [URI parsing](guiutil.cpp) |
| Update translations | [Translation process](../../doc/translation_process.md) · [Qt catalogs](locale/) |
| Run or extend the GUI tests | [Qt test suite](test/) · [Test runner](test/test_main.cpp) |

## Using Qt Creator

Import the configured source tree as an existing project. Select the kit that matches your build compiler and Qt installation. Point the run configuration at the `defcon-qt` executable in your build tree. Open a `.ui` file from the [forms directory](forms/) to edit it in Designer.

See the [contribution guide](../../CONTRIBUTING.md) before submitting a change.
