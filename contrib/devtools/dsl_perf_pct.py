#!/usr/bin/env python3
# Copyright (c) 2026 The Defcon Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Percentiles of the Sentinel latency histograms in a node's debug.log (-dslperf).

With -dslperf, each epoch summary writes one line per latency metric and DKG-cycle position:

  DSL perf hist -- epoch=E base=H metric=M pos=P n=N over=O buckets=k:c,k:c,...

Bucket k holds durations of [2^k, 2^(k+1)) nanoseconds, k = 10..49; shorter ones are in bucket 10, and
those of 2^50 ns or more in bucket 49, where `over` counts them as well. The metrics are tip_queue,
peerman_tip_run, dkg_tip_start, dsl_tick_run and block_msg_wait (src/evo/pose_service_metrics.h).

This script adds the lines up per metric and position, over every epoch in the logs given (or only the
epochs asked for), and prints p50, p90 and p99 conservatively: the upper bound 2^(k+1) ns of the bucket that
holds the sample of that rank. The overflow samples are the largest ones, so a rank among the last `over` is
known only to be at least 2^50 ns: that percentile is a lower bound and is marked with '>='. The text rounds an
upper bound up and a lower bound down, so neither is printed past its exact value; --json gives exact ones.

usage: dsl_perf_pct.py [--epoch E ...] [--metric M ...] [--json] debug.log [debug.log ...]
"""

import argparse
import json
import math
import re
import sys

LINE = re.compile(r"DSL perf hist -- epoch=(\d+) base=([0-9a-f]{64}) metric=([a-z_]+) pos=(\d+) "
                  r"n=(\d+) over=(\d+) buckets=(\d+:\d+(?:,\d+:\d+)*)\s*$")
LOW, HIGH = 10, 49
POSITIONS = 24
QUANTILES = (("p50", 0.50), ("p90", 0.90), ("p99", 0.99))


def parse(lines, epochs=None, metrics=None):
    """{(metric, position): {"n": samples, "over": overflow, "buckets": {k: count}}} from the log lines."""
    table = {}
    for line in lines:
        if "DSL perf hist -- " not in line:
            continue
        match = LINE.search(line)
        if match is None:
            raise ValueError(f"malformed histogram line: {line.strip()}")
        epoch, _base, metric, pos, n, over, buckets = match.groups()
        epoch, pos, n, over = int(epoch), int(pos), int(n), int(over)
        if (epochs and epoch not in epochs) or (metrics and metric not in metrics):
            continue
        if pos >= POSITIONS:
            raise ValueError(f"position {pos} out of range: {line.strip()}")
        counts = {}
        for item in buckets.split(","):
            k, count = (int(x) for x in item.split(":"))
            if not LOW <= k <= HIGH or count <= 0 or k in counts:
                raise ValueError(f"bad bucket {item}: {line.strip()}")
            counts[k] = count
        if sum(counts.values()) != n or over > counts.get(HIGH, 0):
            raise ValueError(f"counts do not add up: {line.strip()}")
        entry = table.setdefault((metric, pos), {"n": 0, "over": 0, "buckets": {}})
        entry["n"] += n
        entry["over"] += over
        for k, count in counts.items():
            entry["buckets"][k] = entry["buckets"].get(k, 0) + count
    return table


def percentile(entry, q):
    """(bound in ns, is a lower bound) for the sample of rank ceil(q * n): the upper bound 2^(k+1) ns of its
    bucket, or, for one of the `over` largest samples (all in the top bucket, at least 2^50 ns), the lower
    bound 2^50 ns."""
    rank = max(1, math.ceil(q * entry["n"]))
    if rank > entry["n"] - entry["over"]:
        return 2 ** (HIGH + 1), True
    seen = 0
    for k in sorted(entry["buckets"]):
        seen += entry["buckets"][k]
        if seen >= rank:
            return 2 ** (k + 1), False
    raise ValueError("rank beyond the samples")


def report(table):
    rows = []
    for (metric, pos), entry in sorted(table.items()):
        row = {"metric": metric, "pos": pos, "n": entry["n"], "over": entry["over"]}
        for name, q in QUANTILES:
            bound, lower = percentile(entry, q)
            row[name + "_ns"] = bound
            row[name + "_lower_bound"] = lower
        rows.append(row)
    return rows


def human(ns, lower):
    """Three significant digits, rounded away from the side the bound limits: an upper bound up, a lower one
    down, so the printed value never claims more than the exact bound does."""
    mark = ">=" if lower else "<="
    for unit, scale in (("s", 10 ** 9), ("ms", 10 ** 6), ("us", 10 ** 3), ("ns", 1)):
        if ns >= scale or scale == 1:
            digits = len(str(ns // scale))  # integer digits in this unit
            decimals = max(0, 3 - digits)
            step = scale // 10 ** decimals if scale >= 10 ** decimals else 1
            units = ns // step if lower else -(-ns // step)  # floor, or ceiling, in steps of the last digit
            return f"{mark}{units * step / scale:.{decimals}f}{unit}"
    raise AssertionError("unreachable")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--epoch", type=int, action="append", help="only this epoch (repeatable)")
    parser.add_argument("--metric", action="append", help="only this metric (repeatable)")
    parser.add_argument("--json", action="store_true", help="print the rows as JSON")
    parser.add_argument("logs", nargs="+")
    args = parser.parse_args(argv)
    lines = []
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines.extend(f)
    try:
        rows = report(parse(lines, set(args.epoch or []), set(args.metric or [])))
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    if not rows:
        print("no DSL perf hist lines (is -dslperf on?)", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(rows, indent=1))
        return 0
    print(f"{'metric':<16} {'pos':>3} {'n':>7}  {'p50':>10} {'p90':>10} {'p99':>10}")
    for row in rows:
        cells = [human(row[name + "_ns"], row[name + "_lower_bound"]) for name, _ in QUANTILES]
        print(f"{row['metric']:<16} {row['pos']:>3} {row['n']:>7}  {cells[0]:>10} {cells[1]:>10} {cells[2]:>10}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
