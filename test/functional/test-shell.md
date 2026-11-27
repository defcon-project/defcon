# Interactive functional tests

[TestShell](test_framework/test_shell.py) exposes the project's Python test framework in an interactive session. It starts local `defcond` processes on regtest, provides RPC clients and keeps the nodes running until you call `shutdown()`. The inherited base class is named `BitcoinTestFramework`.

## Start a session

Use a configured source build with the functional-test dependencies installed, as described in the [test guide](../README.md). Start Python from the repository root:

```python
import sys
sys.path.insert(0, "test/functional")
from test_framework.test_shell import TestShell

test = TestShell().setup(num_nodes=1, setup_clean_chain=True)
try:
    print(test.nodes[0].getblockchaininfo()["chain"])
    print(test.nodes[0].getblockcount())
finally:
    test.shutdown()
```

The clean chain starts at its genesis block. `setup()` accepts framework and option attributes as keyword arguments. A process can run one shell at a time. Set `test.options.nocleanup = True` before shutdown to retain the logs. Call `test.reset()` after shutdown to restore defaults before a new session.

## Setup options

Use keyword arguments when starting the shell. For example, `TestShell().setup(num_nodes=2, setup_clean_chain=True, loglevel="DEBUG")` starts two nodes on a fresh regtest chain. The [framework](test_framework/test_framework.py) defines the full set of options.

| Option | Default | Use |
| --- | --- | --- |
| `num_nodes` | `1` | Number of node processes in this session. |
| `setup_clean_chain` | `False` | Set to `True` to start at genesis. The default uses the cached regtest fixture. |
| `tmpdir` | Generated temporary directory | Provide a new path for this session's data and logs. |
| `cachedir` | `test/cache` | Select the cache of pregenerated node directories. |
| `nocleanup` | `False` | Set to `True` to retain the test directory after shutdown. |
| `loglevel`, `trace_rpc` | `"INFO"`, `False` | Adjust console logging and optionally log RPC calls. |
| `randomseed` | Selected at startup | Supply an integer to repeat the framework's pseudorandom sequence. |
| `rpc_timeout` | `60` seconds | Set the node RPC timeout used by the framework. |
| `usecli` | `False` | Use the built `defcon-cli` where the framework supports it. |
| `perf` | `False` | Enable Linux node profiling with `perf` and retain the profile data. |

## Further reading

The [functional test guide](README.md) describes how to turn an experiment into a test. The helpers for [keys](test_framework/key.py), [scripts](test_framework/script.py) and [network messages](test_framework/messages.py) are the same ones used by the suite.
