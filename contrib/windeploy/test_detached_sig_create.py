#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise the signing helper with disposable inputs and no signing keys."""

import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('detached-sig-create.sh')
TOOL = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
with Path('calls.jsonl').open('a', encoding='utf-8') as log:
    log.write(json.dumps(args) + '\n')
mode = os.environ.get('SIGN_TEST_MODE', '')
operation = args[0]
source = Path(args[args.index('-in') + 1])
output = Path(args[args.index('-out') + 1])
if mode == operation + '-fail':
    sys.exit(7 if operation == 'sign' else 8)
if mode == 'second-sign-fail' and 'second' in source.name:
    sys.exit(7)
if mode == operation + '-missing':
    sys.exit(0)
if operation == 'sign':
    output.write_bytes(b'signed:' + source.read_bytes())
else:
    assert source.read_bytes().startswith(b'signed:')
    output.write_bytes(b'' if mode == 'empty-signature' else b'fixture-signature')
'''


class DetachedSignTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='defcon-sign-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'unsigned').mkdir()
        (self.root / 'tools').mkdir()
        self.tool = self.root / 'tools' / 'mock signer'
        self.tool.write_text(TOOL, encoding='utf-8')
        self.tool.chmod(0o755)
        self.input('app-unsigned.exe')
        self.archive = self.root / 'signature-win.tar.gz'
        self.archive.write_bytes(b'preserved archive')
        # The helper must clean only its own unique temporary directory.
        (self.root / '.tmp').mkdir()
        (self.root / '.tmp' / 'keep').write_bytes(b'unrelated')

    def input(self, name):
        (self.root / 'unsigned' / name).write_bytes(b'unsigned fixture')

    def run_helper(self, mode='', args=None):
        env = dict(os.environ, OSSLSIGNCODE=str(self.tool), SIGN_TEST_MODE=mode)
        env['PATH'] = str(self.root / 'tools') + os.pathsep + env['PATH']
        shell = env.get('SIGN_TEST_SHELL', '/bin/sh')
        return subprocess.run([shell, str(SCRIPT), *(args if args is not None else
                              ['-key', 'mock key'])], cwd=self.root, env=env,
                              capture_output=True, text=True, timeout=15)

    def assert_failure(self, result):
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertNotIn('Created signature-win.tar.gz', result.stdout)
        self.assertEqual(b'preserved archive', self.archive.read_bytes())
        self.assertEqual(b'unrelated', (self.root / '.tmp' / 'keep').read_bytes())
        self.assertEqual([], list(self.root.glob('.tmp.*')))

    def fail_command(self, name, code):
        tool = self.root / 'tools' / name
        tool.write_text('#!/bin/sh\nexit ' + str(code) + '\n', encoding='utf-8')
        tool.chmod(0o755)

    def test_sign_failure_stops_before_extraction(self):
        self.assert_failure(self.run_helper('sign-fail'))
        calls = (self.root / 'calls.jsonl').read_text(encoding='utf-8').splitlines()
        self.assertEqual(['sign'], [json.loads(c)[0] for c in calls])

    def test_extract_failure_does_not_publish(self):
        self.assert_failure(self.run_helper('extract-signature-fail'))

    def test_success_without_signed_output_is_rejected(self):
        self.assert_failure(self.run_helper('sign-missing'))

    def test_success_without_signature_is_rejected(self):
        self.assert_failure(self.run_helper('extract-signature-missing'))

    def test_empty_signature_is_rejected(self):
        self.assert_failure(self.run_helper('empty-signature'))

    def test_missing_inputs_are_rejected(self):
        (self.root / 'unsigned' / 'app-unsigned.exe').unlink()
        self.assert_failure(self.run_helper())
        self.assertFalse((self.root / 'calls.jsonl').exists())

    def test_missing_arguments_are_rejected(self):
        self.assert_failure(self.run_helper(args=[]))

    def test_later_failure_cannot_publish_a_partial_archive(self):
        self.input('second-unsigned.exe')
        self.assert_failure(self.run_helper('second-sign-fail'))

    def test_archive_failure_preserves_previous_output(self):
        self.fail_command('tar', 9)
        self.assert_failure(self.run_helper())

    def test_publish_failure_preserves_previous_output(self):
        self.fail_command('mv', 10)
        self.assert_failure(self.run_helper())

    def test_temp_creation_failure_preserves_previous_output(self):
        self.fail_command('mktemp', 11)
        self.assert_failure(self.run_helper())

    def test_success_preserves_names_arguments_and_only_publishes_signatures(self):
        self.input('app with spaces-unsigned.exe')
        result = self.run_helper()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('Created signature-win.tar.gz', result.stdout)
        with tarfile.open(self.archive, 'r:gz') as archive:
            files = [m for m in archive.getmembers() if m.isfile()]
            self.assertEqual({'./win/app-unsigned.exe.pem',
                              './win/app with spaces-unsigned.exe.pem'},
                             {m.name for m in files})
            for member in files:
                self.assertEqual(b'fixture-signature', archive.extractfile(member).read())
        calls = [json.loads(c) for c in
                 (self.root / 'calls.jsonl').read_text(encoding='utf-8').splitlines()]
        signs = [c for c in calls if c[0] == 'sign']
        self.assertEqual(2, len(signs))
        self.assertTrue(all(c[-2:] == ['-key', 'mock key'] for c in signs))
        self.assertEqual(b'unrelated', (self.root / '.tmp' / 'keep').read_bytes())
        self.assertEqual([], list(self.root.glob('.tmp.*')))
        for name in ('app-unsigned.exe', 'app with spaces-unsigned.exe'):
            self.assertEqual(b'unsigned fixture', (self.root / 'unsigned' / name).read_bytes())


if __name__ == '__main__':
    unittest.main(verbosity=2)
