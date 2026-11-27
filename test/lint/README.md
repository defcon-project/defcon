# Source lint checks

Run [all-lint.py](all-lint.py) from the repository root to invoke the scripts matching its lint pattern:

```sh
python3 test/lint/all-lint.py
```

The [lint environment script](../../ci/lint/04_install.sh) records the external tools used by these checks.

| Tool | Purpose |
| --- | --- |
| [check-doc.py](check-doc.py) | Checks the documentation of command-line options. |
| [commit-script-check.sh](commit-script-check.sh) | Verifies [scripted diffs](../../doc/developer-notes.md#scripted-diffs). |
| [git-subtree-check.sh](git-subtree-check.sh) | Compares an imported subtree with its recorded Git tree. |

```sh
test/lint/git-subtree-check.sh src/secp256k1
```

For the additional upstream-commit check, make the relevant upstream Git object available locally and pass `-r`. Use the repository and revision recorded for that particular import.
