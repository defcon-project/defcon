#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The hosted unit gate: every suite of the test binary but the named exceptions, several at a time.

  run --binary B [--exclude A:B/case] [--split SUITE] [--jobs N] [-- ARGS]

The binary lists its own suites (--list_content), so a suite added tomorrow runs the day it lands. Each suite
runs in a process of its own, --jobs of them at a time; one process for all of them ran them one after another
on a runner with four CPUs. --exclude takes the exceptions, a suite or one case of a suite (suite/case), separated by
colons. --split names a suite whose cases run one per process, for a suite a single process would hold longer
than everything else together; those start first. ARGS go to every process.

Nothing is taken on trust. An exception or a split that names nothing in the binary is refused, and so is a
listing this cannot read. Every process must exit 0 and must report having run exactly the cases the listing
holds for its selection, so a selection that matched fewer cases (or none) fails the gate.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True
# A line of --list_content: four spaces a level, the name, and a star when the unit runs by default.
LISTED = re.compile(r'(?P<indent>(?:    )*)(?P<name>[A-Za-z0-9_]+)(?P<enabled>\*?)')
RAN = re.compile(r'^Running ([0-9]+) test cases?\.\.\.$', re.MULTILINE)
ANSI = re.compile(r'\x1b\[[0-9;]*m')


class GateError(ValueError):
    """The listing or a request about it cannot be used."""


class Unit:
    """One process: the filter it runs and the number of cases that filter selects."""

    def __init__(self, selection, cases):
        self.selection = selection
        self.cases = cases


def read_listing(text):
    """--list_content as a tree: {suite: {case: {}}}, in the binary's order. Leaves are cases."""
    tree, path = {}, []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        match = LISTED.fullmatch(line.rstrip())
        if not match:
            raise GateError('listing line {}: cannot read {!r}'.format(number, line))
        depth = len(match.group('indent')) // 4
        if depth > len(path):
            raise GateError('listing line {}: {!r} is indented past its parent'.format(number, line))
        name = match.group('name')
        if not match.group('enabled'):
            # A filter that names a unit can start one that is off by default; the old single run did not.
            raise GateError('listing line {}: {} is off by default, which this gate does not handle'.format(number, name))
        del path[depth:]
        siblings = tree
        for parent in path:
            siblings = siblings[parent]
        if name in siblings:
            raise GateError('listing line {}: {} is listed twice'.format(number, name))
        siblings[name] = {}
        path.append(name)
    if not tree:
        raise GateError('the binary lists no test')
    return tree


def count(node):
    """Cases under a unit of the tree; a unit without children is one case."""
    return sum(count(child) for child in node.values()) if node else 1


def plan(tree, excluded=(), split=()):
    """The processes to start, in order: the split suites' cases first, then every other suite as listed."""
    whole, cases = set(), {}
    for entry in excluded:
        suite, slash, case = entry.partition('/')
        if suite not in tree or (slash and case not in tree[suite]):
            raise GateError('the exception {} names nothing in the binary'.format(entry))
        if not slash:
            whole.add(suite)
        else:
            cases.setdefault(suite, []).append(case)
    if whole & set(cases):
        raise GateError('{} is excepted both whole and by case'.format(', '.join(sorted(whole & set(cases)))))
    for suite, names in cases.items():
        if len(names) != len(set(names)) or len(names) == len(tree[suite]):
            raise GateError('the exceptions of {} repeat a case or leave it none; except the suite'.format(suite))
    if len(split) != len(set(split)):
        raise GateError('a suite is split twice')
    for suite in split:
        if suite not in tree or suite in whole or not tree[suite]:
            raise GateError('the split suite {} is not a suite this gate runs'.format(suite))
    first, rest = [], []
    for suite, children in tree.items():
        if suite in whole:
            continue
        dropped = cases.get(suite, [])
        if suite in split:
            first += [Unit(suite + '/' + case, count(node)) for case, node in children.items() if case not in dropped]
        else:
            selection = ':'.join([suite] + ['!{}/{}'.format(suite, case) for case in dropped])
            rest.append(Unit(selection, count(children) - sum(count(children[case]) for case in dropped)))
    if not first + rest:
        raise GateError('the exceptions leave nothing to run')
    return first + rest


def start(binary, arguments, unit):
    """Run one unit; (unit, seconds, why it failed or None, its output)."""
    began = time.monotonic()
    try:
        result = subprocess.run([binary, '--run_test=' + unit.selection] + list(arguments),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    except OSError as error:
        return unit, time.monotonic() - began, 'did not start: {}'.format(error), ''
    output = result.stdout.decode('utf-8', errors='replace')
    ran = RAN.findall(ANSI.sub('', output))
    failure = None
    if result.returncode != 0:
        failure = 'exit {}'.format(result.returncode)
    elif ran != [str(unit.cases)]:
        failure = 'ran {} test cases, the listing holds {}'.format(' and '.join(ran) or 'no', unit.cases)
    return unit, time.monotonic() - began, failure, output


def run(units, jobs, runner, out=sys.stdout):
    """Run every unit, `jobs` at a time, in order; print each as it ends. Returns [(unit, seconds, failure)]."""
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for future in concurrent.futures.as_completed([pool.submit(runner, unit) for unit in units]):
            unit, seconds, failure, output = future.result()
            results.append((unit, seconds, failure))
            if failure:
                # Not folded away: a failure is what the log is opened for.
                print('FAILED {} ({}, {:.0f} s)'.format(unit.selection, failure, seconds), file=out)
                print(output, file=out)
            else:
                print('::group::{} ({} cases, {:.0f} s)'.format(unit.selection, unit.cases, seconds), file=out)
                print(output, file=out)
                print('::endgroup::', file=out)
            out.flush()
    return results


def summary(results, wall):
    failed = [(unit, failure) for unit, _, failure in results if failure]
    lines = ['### Unit suites', '',
             '{} processes, {} test cases, {:.0f} s of tests in {:.0f} s; {} failed.'.format(
                 len(results), sum(unit.cases for unit, _, _ in results),
                 sum(seconds for _, seconds, _ in results), wall, len(failed)), '']
    for unit, failure in failed:
        lines.append('- FAILED `{}`: {}'.format(unit.selection, failure))
    lines += ['', '| Longest | Seconds |', '| --- | ---: |']
    for unit, seconds, _ in sorted(results, key=lambda result: -result[1])[:10]:
        lines.append('| {} | {:.0f} |'.format(unit.selection, seconds))
    return '\n'.join(lines + [''])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    command = sub.add_parser('run')
    command.add_argument('--binary', required=True)
    command.add_argument('--exclude', default='', help='suite or suite/case, separated by colons')
    command.add_argument('--split', action='append', default=[], help='a suite whose cases run one per process')
    command.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    command.add_argument('arguments', nargs='*', help='after --, for every process')
    args = parser.parse_args(argv)
    try:
        if args.jobs < 1:
            raise GateError('--jobs takes a positive number')
        listing = subprocess.run([args.binary, '--list_content'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if listing.returncode != 0:
            raise GateError('{} --list_content: exit {}'.format(args.binary, listing.returncode))
        # Boost.Test writes the listing to standard error.
        tree = read_listing(ANSI.sub('', listing.stderr.decode('utf-8', errors='replace')))
        units = plan(tree, [entry for entry in args.exclude.split(':') if entry], args.split)
    except (GateError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')  # a test's output is not always text
    print('{} suites listed; {} processes, {} at a time'.format(len(tree), len(units), args.jobs), flush=True)
    began = time.monotonic()
    results = run(units, args.jobs, lambda unit: start(args.binary, args.arguments, unit))
    text = summary(results, time.monotonic() - began)
    print(text)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        try:
            with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as out:
                out.write(text + '\n')
        except OSError as error:
            print('::warning::the unit suite summary was not written: {}'.format(error))
    return 1 if any(failure for _, _, failure in results) else 0


if __name__ == '__main__':
    sys.exit(main())
