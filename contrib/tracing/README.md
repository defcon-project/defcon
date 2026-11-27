# Trace a DeFCoN node on Linux

These examples attach to USDT tracepoints in a running `defcond`. Use a build with USDT support and a Linux account with permission to attach the probes. The [tracepoint reference](../../doc/tracing.md) lists their arguments and how to check which ones a binary exposes.

Run these examples from the repository root. The bpftrace scripts refer to `./src/defcond` inside the script. Change that path if the running executable is elsewhere. The Python examples use the BCC bindings and accept the binary path as an argument.

| Example | What it reports |
| --- | --- |
| [log_p2p_traffic.bt](log_p2p_traffic.bt) | Peer, message type and byte count for incoming and outgoing messages. |
| [p2p_monitor.py](p2p_monitor.py) | Interactive per-peer traffic statistics. |
| [log_raw_p2p_msgs.py](log_raw_p2p_msgs.py) | Message metadata and payload bytes, truncated at the script’s capture limit. |
| [connectblock_benchmark.bt](connectblock_benchmark.bt) | Block connection times by height range and reporting threshold. |
| [log_utxocache_flush.py](log_utxocache_flush.py) | UTXO cache flush events. |
| [log_utxos.bt](log_utxos.bt) | Individual additions, spends and removals in the UTXO cache. |

```sh
bpftrace contrib/tracing/log_p2p_traffic.bt
python3 contrib/tracing/p2p_monitor.py ./src/defcond
```

See the [bpftrace documentation](https://bpftrace.org/docs/) and the [BCC project](https://github.com/iovisor/bcc) for installation and platform requirements.
