#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The hosted functional gate: its order, its shards, and what a run measured.

The list lives in ci/dash/functional_gate.txt, one test per line: the seconds all
of its variants took on the hosted runner, the test, and what it guards.

  order [--shard I/N]  print the tests longest first (or shard I of N of them)
  report LOG           compare the seconds a test_runner.py log measured with the list

test_runner.py starts tests in the order it is given them, so a long test that
starts late holds the lane after everything else has finished. Ordering by the
recorded seconds keeps the longest first however the list grows.
"""

import argparse
import importlib.util
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / 'ci/dash/functional_gate.txt'
TESTS_DIR = ROOT / 'test/functional'
ENTRY = re.compile(r'(?P<seconds>[1-9][0-9]*) +(?P<name>[A-Za-z0-9_]+\.py) +# +(?P<guards>\S.*)')
# A row of test_runner.py's closing table: "<test> [--args] | <status> | <seconds> s", also behind the
# timestamp a downloaded GitHub log puts in front of every line.
RESULT = re.compile(r'(?:^|\s)(?P<test>[A-Za-z0-9_]+\.py)(?: [^|]*?)? *\| *(?P<status>[^|]*?) *\| *(?P<seconds>[0-9]+) s *$')
# Colours, should the runner ever print them (it does only on a terminal).
ANSI = re.compile(r'\x1b\[[0-9;]*m')


class GateError(ValueError):
    """The gate list or a request about it is malformed."""


def read_gate(path=GATE):
    """Return [(name, seconds)] in file order."""
    entries, seen = [], set()
    for number, line in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        match = ENTRY.fullmatch(line.rstrip())
        if not match:
            raise GateError('{}:{}: expected "<seconds> <test>.py  # what it guards"'.format(path, number))
        name = match.group('name')
        if name in seen:
            raise GateError('{}:{}: {} is listed twice'.format(path, number, name))
        seen.add(name)
        entries.append((name, int(match.group('seconds'))))
    if not entries:
        raise GateError('{}: the gate lists no test'.format(path))
    return entries


def longest_first(entries):
    """Sort by recorded seconds, longest first; equal seconds keep the list's order."""
    return sorted(entries, key=lambda entry: -entry[1])


def shard(entries, index, count):
    """Shard `index` (1-based) of `count`: longest first, each test to the least loaded shard."""
    if not 1 <= index <= count:
        raise GateError('shard {} of {} does not exist'.format(index, count))
    loads, shards = [0] * count, [[] for _ in range(count)]
    for name, seconds in longest_first(entries):
        target = min(range(count), key=lambda i: (loads[i], i))
        loads[target] += seconds
        shards[target].append((name, seconds))
    return shards[index - 1]


def unregistered(names, tests_dir=TESTS_DIR):
    """Names the runner would only warn about: none of its registered scripts starts with them."""
    spec = importlib.util.spec_from_file_location('gate_test_runner', Path(tests_dir) / 'test_runner.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    return [name for name in names if not any(script.startswith(name) for script in runner.ALL_SCRIPTS)]


def measured(log_text):
    """Seconds per test from a test_runner.py log, all variants of a test added up."""
    seconds = {}
    for line in log_text.splitlines():
        match = RESULT.search(ANSI.sub('', line).strip())
        if match:
            name = match.group('test')
            seconds[name] = seconds.get(name, 0) + int(match.group('seconds'))
    return seconds


def report(entries, log_text):
    times = measured(log_text)
    lines = ['### Functional gate: recorded and measured seconds', '']
    if not times:
        return '\n'.join(lines + ['The log holds no closing table of test results.', ''])
    recorded = dict(entries)
    lines += ['| Test | Recorded | Measured | |', '| --- | ---: | ---: | --- |']
    for name in sorted(set(times) | set(recorded), key=lambda n: (-times.get(n, -1), n)):
        then, now = recorded.get(name), times.get(name)
        note = ''
        if then is None:
            note = 'not in the list'
        elif now is None:
            note = 'not run'
        elif abs(now - then) > max(30, then // 2):
            note = 'refresh the recorded seconds'
        lines.append('| {} | {} | {} | {} |'.format(name, '' if then is None else then, '' if now is None else now, note))
    return '\n'.join(lines + ['', 'The list is ci/dash/functional_gate.txt; only the order of the seconds matters.', ''])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--gate', default=str(GATE), help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest='command', required=True)
    order = sub.add_parser('order')
    order.add_argument('--shard', default='1/1', help='I/N, for example 2/3')
    order.add_argument('--tests-dir', default=str(TESTS_DIR), help=argparse.SUPPRESS)
    result = sub.add_parser('report')
    result.add_argument('log')
    args = parser.parse_args(argv)
    try:
        entries = read_gate(args.gate)
        if args.command == 'report':
            print(report(entries, Path(args.log).read_text(encoding='utf-8', errors='replace')))
            return 0
        match = re.fullmatch(r'([1-9][0-9]*)/([1-9][0-9]*)', args.shard)
        if not match:
            raise GateError('--shard takes I/N, for example 2/3')
        missing = unregistered([name for name, _ in entries], args.tests_dir)
        if missing:
            raise GateError('gate entries not registered in test_runner.py: ' + ', '.join(missing))
        selected = shard(entries, int(match.group(1)), int(match.group(2)))
        if not selected:
            raise GateError('shard {} holds no test'.format(args.shard))
    except (GateError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    print('\n'.join(name for name, _ in selected))
    return 0


if __name__ == '__main__':
    sys.exit(main())
