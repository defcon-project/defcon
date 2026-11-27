# Testing DeFCoN Core

[Build DeFCoN Core](../doc/README.md#building) before running tests that use its binaries. Run those tests from the configured build directory for an in-tree build. The configure tests below are the exception. The [hosted CI guide](../ci/README.md) describes which tests run on pull requests.

| Suite | Where to start |
| --- | --- |
| C++ unit and utility tests | `make check`. Unit tests are in [src/test](../src/test/) and [src/wallet/test](../src/wallet/test/). |
| Functional | [Python RPC and P2P tests](functional/README.md) against local `defcond` instances. |
| GUI | Qt test runner and suite selection below. |
| Fuzz | [Fuzzing guide](../doc/fuzzing.md). |
| Configure | [Build-option contract tests](configure/test_configure.py). |
| Source lints | [Lint guide](lint/README.md). |

## Functional tests

Use Python 3 and the `dash_hash` module for the project's X11 block hashes. The [CI image](../contrib/containers/ci/Dockerfile.linux64) pins the version and records the installation commands. ZMQ tests also require `pyzmq`. On Windows, set `PYTHONUTF8=1`.

```sh
python3 test/functional/test_runner.py wallet_hd.py
python3 test/functional/test_runner.py --jobs=4
python3 test/functional/test_runner.py --help
```

The runner accepts multiple test names. Use `--extended` for the extended suite or `--coverage` to list the RPCs from `help` that no test called. The runner counts `generate`, `voteraw` and `debug` as covered without a call; with `--coverage` the run fails if any other RPC is left uncovered. Each test receives temporary node data directories and separate ports. Run one test directly to see its options, or select a family from the runner with a regular expression:

```sh
python3 test/functional/wallet_hd.py --help
python3 test/functional/test_runner.py --filter '^wallet_' --jobs=2
```

The default is four parallel jobs. Use `--tmpdirprefix` on the runner to place temporary directories on another filesystem, including an existing RAM-backed test volume. Size it for the number of parallel nodes. `--keepcache` preserves the existing functional-test cache across runner invocations.

Backward-compatibility tests need earlier release binaries in `releases/` or in the directory set by `PREVIOUS_RELEASES_DIR`. By default they run when that directory exists and is not empty, and are skipped otherwise; once they run, every release binary they start must be present. `test/get_previous_releases.py -b <tag>...` downloads upstream Dash Core release binaries from `dashpay/dash`; the inherited [Qt CI setup](../ci/test/00_setup_env_native_qt5.sh) lists tags in `PREVIOUS_RELEASES_TO_DOWNLOAD`. The hosted CI runs the functional tests in [functional_gate.txt](../ci/dash/functional_gate.txt), which does not include the backward-compatibility tests.

### Test logging

Each run prints its temporary directory. The test log is `test_framework.log` and node logs are under `nodeN/regtest/debug.log`. On failure the framework retains the data directory. To keep it after a successful test, pass `--nocleanup` to the individual test. Merge the logs with:

```sh
python3 test/functional/combine_logs.py /path/to/test-directory
```

For a failed run, identify the specific node by its test data directory before stopping it. A debugger in the test can inspect `self.nodes[i].process.pid`. Pass `--pdbonfailure` to an individual test for an interactive Python traceback, `--tracerpc` to trace calls, or `--combinedlogslen=100` to the runner for the last 100 merged log lines on failure. Linux node profiling is covered in the [functional guide](functional/README.md#benchmarking-with-perf).

## Qt tests

With Qt and tests enabled, run:

```sh
make -C src qt/test/test_defcon-qt
QT_QPA_PLATFORM=minimal ./src/qt/test/test_defcon-qt
QT_QPA_PLATFORM=minimal ./src/qt/test/test_defcon-qt --suite=GUIUtilTests
```

Wallet suites such as `MasternodeListTests` exist only in builds with wallet support.

The default runs every compiled suite. Pass an exact Qt class name to select a suite. Additional QTest arguments select individual test functions. The Linux CI also repeats the masternode filtering and ordering cases with LeakSanitizer. A temporary `XDG_CONFIG_HOME` isolates desktop settings in a local test.

## Configure tests

The configure tests run fresh out-of-tree configure runs. Use a source tree prepared with `./autogen.sh` but not yet configured in place, and provide a new output directory:

```sh
python3 test/configure/test_configure.py --output-dir /tmp/defcon-configure-tests
```

Use `--prefix=/path/to/depends/x86_64-pc-linux-gnu` to select a depends prefix. The test records logs and generated Makefiles in the output directory.
