# DeFCoN Core continuous integration

The [GitHub Actions workflow](../.github/workflows/build.yml) runs on pull requests, pushes to `v22.1.x` and version tags. The scripts under [dash/](dash/) retain their historical directory name.

## What the required check covers

The check named `Build (linux64, x86_64-pc-linux-gnu, linux64)` collects change detection, the CI image, the depends build and the Linux check matrix. For documentation-only pull requests, change detection and scheduling checks run and the workflow skips compilation. Code changes run the following lanes.

| Lane | Checks |
| --- | --- |
| Runtime | Builds DeFCoN Core, runs the unit suites and Qt tests, and checks the selected GUI cases with LeakSanitizer. |
| Functional | Builds the same binaries, then distributes the [functional gate](dash/functional_gate.txt) across three runners. |
| Build options | Checks configure combinations and builds and tests the wallet-disabled configuration. |

Fuzz regression runs as a separate workflow job on code changes. The aggregate build check does not depend on that job. A push to a fork mirrors the upstream build result for the same commit. Pull requests opened in a fork run their own CI.

## Updating the gates

Edit [functional_gate.txt](dash/functional_gate.txt) to add a functional test. The [scheduler](dash/functional_gate.py) checks the runner's list and assigns longer tests first. The [unit suite runner](dash/unit_suites.py) checks the suites reported by the compiled test binary.

From the repository root, run the scheduling regression checks with:

```sh
python3 ci/dash/test_ci_scheduling.py
```

The [container guide](../contrib/containers/README.md) describes the CI images. Runtime and build-options jobs use separate compiler caches. Functional jobs restore the runtime cache. The depends cache follows the dependency recipes and build image.

## Other scripts

[ci/test_run_all.sh](test_run_all.sh) is an inherited stage-preparation entry point. It sources environment, installation and pre-build scripts and can modify local caches. The hosted workflow defines the actual build and test jobs. [retry](retry/retry) is the bundled command-retry utility.
