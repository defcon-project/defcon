# DeFCoN Core fuzz regression tests

DeFCoN Core uses libFuzzer test harnesses to check how components handle saved inputs. The [harnesses](../src/test/fuzz/) cover parsers, serialization, data structures, network message handling and other components. They run against a test build.

## Continuous integration

The [regression job](../ci/dash/fuzz_regression.sh) runs the supported targets against a fixed revision of the [Dash QA corpus](https://github.com/dashpay/qa-assets). The script records the exact revision in `QA_ASSETS_COMMIT`; the [target list](../ci/dash/fuzz_targets.txt) describes the expected harnesses. [Dash-only inputs](../ci/dash/fuzz_qa_assets_dash_only.txt) are filtered out. A supported target with no saved inputs receives one empty input. Consult `EXCLUDED_TARGETS` in the script for currently excluded targets and the reasons.

## Build and run a target

Run from the repository root. The script's `build` step configures the tree in place with `clang-18`, libFuzzer and the sanitizers used in CI, then builds the `src/test/fuzz/fuzz` binary. The script header lists the libraries the build needs. Set `FUZZ` to a name from the target list to run one harness, and stop it with Ctrl-C. Pass a directory after the binary to keep the inputs that reach new code. To reproduce a failure from the CI job, check out the `QA_ASSETS_COMMIT` revision of the corpus and pass the failing input file instead, for example `FUZZ=<target> src/test/fuzz/fuzz <qa-assets>/fuzz_seed_corpus/<target>/<input>`. This runs the input directly, without the CI's sanitizer settings. To check a CI result in the same environment, use `test/fuzz/test_runner.py` as the script's `run` step does; it sets `UBSAN_OPTIONS` and `ASAN_OPTIONS`.

```sh
ci/dash/fuzz_regression.sh build
FUZZ=process_message src/test/fuzz/fuzz
```

## Working with the harnesses

The regression runner for saved fuzz inputs is [`test/fuzz/test_runner.py`](../test/fuzz/test_runner.py). It reads `test/config.ini` from the build and accepts a corpus directory and optional target names. See `--help` for the full list of arguments. Use a regression input that exercises the relevant component and record the source revision, target and outcome when reporting a test result.

The [Bitcoin Core QA corpus](https://github.com/bitcoin-core/qa-assets) is an upstream resource for shared code. Report DeFCoN-specific issues through this project's [contributing guide](../CONTRIBUTING.md) or [security policy](../SECURITY.md).
