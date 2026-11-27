# DeFCoN Core unit tests

These tests use Boost.Test. The common test setup is in [util/setup_common.cpp](util/setup_common.cpp).

## Build and run

After configuring with tests enabled, run `make check` from the build root. To build and run just the core unit suite, first build the test binary, then run `make -C src check-unit`. For Qt tests, use `src/qt/test/test_defcon-qt` in a build with Qt tests enabled.

To rebuild the core test binary after changing a test or one of its dependencies, run the following from the build root.

```sh
make -C src test/test_defcon
```

## Run individual cases

List available suites or select a single case by its Boost.Test name.

```sh
src/test/test_defcon --list_content
src/test/test_defcon --run_test=getarg_tests/doubledash
```

For verbose output, use `--log_level=all`. Options after `--` go to the test fixture, for example `src/test/test_defcon --run_test=getarg_tests -- -printtoconsole=1`. The make target writes a log for each test source in the corresponding build directory, such as `src/test/getarg_tests.log`.

## Add or debug a test

Add a Boost.Test case to the relevant source file. Register a new C++ test file in [Makefile.test.include](../Makefile.test.include). GUI tests are registered in the [Qt test makefile](../Makefile.qttest.include) and [Qt test runner](../qt/test/test_main.cpp).

To run a case in a debugger, use a debug build and launch `gdb --args src/test/test_defcon --run_test=getarg_tests/doubledash`. Inside GDB, use `run` to start and `bt` to inspect a stopped thread's backtrace.
