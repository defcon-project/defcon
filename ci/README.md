## CI Scripts

This directory contains scripts for each build step in each build stage.

The active GitHub Actions pipeline is defined in
[`build.yml`](../.github/workflows/build.yml). It uses `ci/dash/matrix.sh` and
`ci/dash/build_src.sh`; dependency builds, unit tests and the functional gate
are invoked by the workflow itself. The local stage runner below is a separate
entry point.

### GitHub Actions lanes

After the image and dependency prefix are ready, two jobs run on separate
GitHub-hosted machines. The `runtime` lane builds the regular binaries and
runs the existing unit, Qt/LSan and functional gates. The `build-options` lane
runs the configure checks and the wallet-disabled build and regressions.
Neither lane removes tests or changes their concurrency or timeout settings.

The required check remains `Build (linux64, x86_64-pc-linux-gnu, linux64)`.
It waits for both lanes and rejects failed, cancelled, skipped or missing
prerequisites. Documentation-only changes still receive that check. No branch
protection setting needs to be changed.

Each lane persists its own compiler cache, bound to the dependency/image
identity, in `/cache/ccache`. The directory is set at job scope so later shell
steps, including the no-wallet build, use the restored cache too. The no-wallet
build has a stable path inside each fresh job; build outputs and test results
are not reused. Cache statistics appear in the job summary even after failure.
The runtime lane can also seed its cache from the previous cache-key format;
the no-wallet lane starts with an empty cache on its first run.

Run the no-build scheduling and required-check regressions with:

```
python3 ci/dash/test_ci_scheduling.py
```

For a local container runner, preserve the compiler cache across runs by mounting
a dedicated cache directory at `/cache` and setting `CACHE_DIR=/cache` and
`CCACHE_DIR=/cache/ccache` for **every** stage. Keep a fixed container-side source
and build path, use the same image/compiler/configuration, and retain all normal
test gates. Do not share writable build directories or run a second heavy local
job merely because GitHub uses separate machines. A clean source/build directory
does not require an empty compiler cache.

### Running a Stage Locally

Be aware that the tests will be built and run in-place, so please run at your own risk.
If the repository is not a fresh git clone, you might have to clean files from previous builds or test runs first.

The ci needs to perform various sysadmin tasks such as installing packages or writing to the user's home directory.
While most of the actions are done inside a docker container, this is not possible for all. Thus, cache directories,
such as the depends cache, previous release binaries, or ccache, are mounted as read-write into the docker container. While it should be fine to run
the ci system locally on you development box, the ci scripts can generally be assumed to have received less review and
testing compared to other parts of the codebase. If you want to keep the work tree clean, you might want to run the ci
system in a virtual machine with a Linux operating system of your choice.

To allow for a wide range of tested environments, but also ensure reproducibility to some extent, the test stage
requires `docker` to be installed. To install all requirements on Ubuntu, run

```
sudo apt install docker.io bash
```

To run the default test stage,

```
./ci/test_run_all.sh
```

To run the test stage with a specific configuration,

```
FILE_ENV="./ci/test/00_setup_env_arm.sh" ./ci/test_run_all.sh
```

### Configurations

The test files (`FILE_ENV`) are constructed to test a wide range of
configurations, rather than a single pass/fail. This helps to catch build
failures and logic errors that present on platforms other than the ones the
author has tested.

Some builders use the dependency-generator in `./depends`, rather than using
the system package manager to install build dependencies. This guarantees that
the tester is using the same versions as the release builds, which also use
`./depends`.

If no `FILE_ENV` has been specified or values are left out, `00_setup_env.sh`
is used as the default configuration with fallback values.

It is also possible to force a specific configuration without modifying the
file. For example,

```
MAKEJOBS="-j1" FILE_ENV="./ci/test/00_setup_env_arm.sh" ./ci/test_run_all.sh
```

The files starting with `0n` (`n` greater than 0) are the scripts that are run
in order.

### Cache

In order to avoid rebuilding all dependencies for each build, the binaries are
cached and re-used when possible. Changes in the dependency-generator will
trigger cache-invalidation and rebuilds as necessary.
