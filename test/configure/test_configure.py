#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Check configure option contracts in fresh out-of-tree build directories.

Run autogen.sh first. Requires the native build dependencies; --prefix can
select a depends prefix. Logs and generated files remain in --output-dir.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configure', type=Path, default=Path(__file__).resolve().parents[2] / 'configure')
    parser.add_argument('--output-dir', type=Path, required=True, help='new directory for logs and builds')
    parser.add_argument('--prefix', type=Path, help='optional dependency prefix')
    args = parser.parse_args()
    configure = args.configure.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    common = [str(configure), '--without-gui', '--disable-wallet', '--without-miniupnpc',
              '--without-natpmp', '--disable-zmq', '--disable-tests', '--disable-bench',
              '--disable-fuzz-binary', '--without-daemon', '--without-utils', '--disable-ccache']
    if args.prefix:
        common.append(f'--prefix={args.prefix.resolve()}')
    # Use an executable target for coverage checks so the no-target guard
    # cannot hide a failure in the independent coverage option.
    cases = [
        ('lcov-default', ['--with-libs=no', '--enable-util-tx'], 0, ''),
        ('lcov-disable', ['--with-libs=no', '--enable-util-tx', '--disable-lcov-branch-coverage'], 0, ''),
        ('lcov-enable', ['--with-libs=no', '--enable-util-tx', '--enable-lcov-branch-coverage'], 0, '--rc lcov_branch_coverage=1'),
        ('lcov-explicit-no', ['--with-libs=no', '--enable-util-tx', '--enable-lcov-branch-coverage=no'], 0, ''),
        ('lcov-explicit-yes', ['--with-libs=no', '--enable-util-tx', '--enable-lcov-branch-coverage=yes'], 0, '--rc lcov_branch_coverage=1'),
        ('library-only', ['--with-libs=yes', '--without-boost'], 0, ''),
        ('library-default', ['--without-boost'], 0, ''),
        ('no-targets', ['--with-libs=no'], 1, None),
    ]
    env = os.environ.copy()
    env['LC_ALL'] = 'C.UTF-8'
    results = []
    for name, options, expected_exit, expected_lcov in cases:
        build = output / name
        build.mkdir()
        command = common + options
        start = time.monotonic()
        with (build / 'stdout.log').open('w', encoding='utf-8') as stdout, (build / 'stderr.log').open('w', encoding='utf-8') as stderr:
            process = subprocess.run(command, cwd=build, env=env, stdout=stdout, stderr=stderr, timeout=600, check=False)
        passed = process.returncode == expected_exit
        lcov = None
        if expected_exit == 0 and process.returncode == 0:
            lines = (build / 'Makefile').read_text(encoding='utf-8').splitlines()
            values = [line.partition('=')[2].strip() for line in lines if line.startswith('LCOV_OPTS =')]
            lcov = values[0] if len(values) == 1 else None
            passed = passed and lcov == expected_lcov
            if name.startswith('library-'):
                src_makefile = (build / 'src/Makefile').read_text(encoding='utf-8').splitlines()
                passed = passed and 'LIBBITCOINCONSENSUS = libdashconsensus.la' in src_makefile
                passed = passed and 'lib_LTLIBRARIES = $(LIBBITCOINCONSENSUS)' in src_makefile
        elif expected_exit == 1:
            passed = passed and 'No targets!' in (build / 'stderr.log').read_text(encoding='utf-8')
        results.append({'name': name, 'command': command, 'exit': process.returncode,
                        'expected_exit': expected_exit, 'lcov_opts': lcov,
                        'expected_lcov_opts': expected_lcov, 'elapsed_seconds': time.monotonic() - start,
                        'passed': passed})
        (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        print(f"{name}: {'PASS' if passed else 'FAIL'} (configure exit {process.returncode})", flush=True)
    return 0 if all(result['passed'] for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
