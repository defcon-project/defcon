# DeFCoN Core developer tools

Run the commands below from the repository root.

## clang-format-diff.py

[clang-format-diff.py](clang-format-diff.py) applies the [source formatting rules](../../src/.clang-format) to changed lines. Run without `-i` to inspect the suggested changes.

```sh
git diff -U0 HEAD~1 HEAD | python3 contrib/devtools/clang-format-diff.py -p1
```

## gen-manpages.py

[gen-manpages.py](gen-manpages.py) uses the compiled release binaries and `help2man` to generate the manual pages in [doc/man](../../doc/man/). For an out-of-tree build:

```sh
BUILDDIR="$PWD/build" python3 contrib/devtools/gen-manpages.py
```

## Release binary checks

[security-check.py](security-check.py) checks executable hardening. [symbol-check.py](symbol-check.py) checks the allowed library dependencies and platform symbol versions. Pass the binary paths as arguments. The project tracks its published archives in the [release checklist](../../doc/release-process.md).

## Source maintenance

- [circular-dependencies.py](circular-dependencies.py) finds cycles in the C++ include graph.
- [optimize-pngs.py](optimize-pngs.py) uses `pngcrush` to optimize PNG assets.
- [copyright_header.py](copyright_header.py) can report existing notices. Its inherited update and insert modes target the literal holder `The Dash Core developers`. Preserve authorship when editing source headers.
- [update-translations.py](update-translations.py) post-processes the local locale files; its download step is disabled. See the [translation process](../../doc/translation_process.md) for the project's workflow.
- [github-merge.py](github-merge.py) is a maintainer tool for constructing and signing a local merge. It reads the target repository from `githubmerge.repository` and can update branches and push the result. Contributor submissions follow [CONTRIBUTING.md](../../CONTRIBUTING.md).

## Sentinel latency

[dsl_perf_pct.py](dsl_perf_pct.py) reports percentile bucket bounds for Sentinel latency data written with `-dslperf`:

```sh
python3 contrib/devtools/dsl_perf_pct.py ~/.defcon/debug.log
```
