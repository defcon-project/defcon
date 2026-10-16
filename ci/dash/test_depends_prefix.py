#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""No-build regressions for the prefix transport and rejection boundary."""

import copy
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import depends_prefix as dp


class PrefixTransportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='depends-prefix-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / 'depends' / dp.HOST
        (self.prefix / 'share').mkdir(parents=True)
        (self.prefix / 'share/config.site').write_text('prefix fixture\n')
        (self.prefix / '.stamp_fixture').touch()
        (self.prefix / 'native/bin').mkdir(parents=True)
        (self.prefix / 'native/bin/tool').write_bytes(b'#!/bin/sh\nexit 0\n')
        (self.prefix / 'native/bin/tool').chmod(0o755)
        (self.prefix / 'empty').mkdir()
        (self.prefix / 'native/tool-link').symlink_to('bin/tool')
        (self.prefix / 'native/absolute-tool').symlink_to(str(self.prefix / 'native/bin/tool'))
        self.identity = {'schema': dp.SCHEMA, 'prefix': str(self.prefix), 'fixture': 'input-A',
                         'image': 'ghcr.io/example/image@sha256:' + 'a' * 64}
        self.binding = {'schema': dp.SCHEMA, 'run': '123', 'attempt': '1',
                        'host': dp.HOST, 'source': 'a' * 40,
                        'input_sha256': dp.digest(dp.canonical(self.identity)),
                        'context': {'GITHUB_RUN_ATTEMPT': '1'}}
        self.bundle = self.root / 'bundle'
        self.manifest = dp.prefix_manifest(self.prefix)
        self.envelope_hash = dp.pack(self.prefix, self.bundle, self.identity, self.binding)
        # Preserve the producer fixture while the receiver starts without a prefix.
        self.prefix.rename(self.root / 'producer-preserved')

    def receive(self, identity=None, binding=None):
        return dp.receive(self.bundle, self.prefix, identity or self.identity,
                          binding or self.binding, self.envelope_hash)

    def reject_before_configure(self, callback):
        configured = self.root / 'configure-entered'
        with self.assertRaises(dp.ContractError):
            callback()
            configured.touch()
        self.assertFalse(configured.exists())
        self.assertFalse(self.prefix.exists())
        self.assertEqual([], list(self.prefix.parent.glob('.depends-receive-*')))

    def reseal(self):
        envelope = dp.read_json(self.bundle / 'envelope.json')
        for name, row in envelope['files'].items():
            row.update(sha256=dp.file_digest(self.bundle / name), bytes=(self.bundle / name).stat().st_size)
        dp.write_json(self.bundle / 'envelope.json', envelope)
        self.envelope_hash = dp.file_digest(self.bundle / 'envelope.json')

    def replace_archive(self, transform):
        entries = []
        with tarfile.open(self.bundle / 'prefix.tar.gz', 'r:gz') as archive:
            for member in archive:
                data = archive.extractfile(member).read() if member.isfile() else None
                entries.append((copy.copy(member), data))
        transform(entries)
        with tarfile.open(self.bundle / 'prefix.tar.gz', 'w:gz') as archive:
            for member, data in entries:
                archive.addfile(member, io.BytesIO(data) if data is not None else None)
        self.reseal()

    def test_complete_round_trip_preserves_content_modes_links_and_hidden_stamp(self):
        self.assertEqual(dp.digest(dp.canonical(self.manifest)), self.receive())
        self.assertEqual(self.manifest, dp.prefix_manifest(self.prefix))
        self.assertEqual(0o755, (self.prefix / 'native/bin/tool').stat().st_mode & 0o777)
        self.assertTrue((self.prefix / '.stamp_fixture').is_file())
        self.assertTrue((self.prefix / 'empty').is_dir())
        self.assertEqual('bin/tool', os.readlink(self.prefix / 'native/tool-link'))

    def test_wrong_input_rejected(self):
        value = dict(self.identity, fixture='input-B')
        self.reject_before_configure(lambda: self.receive(identity=value))

    def test_wrong_run_attempt_host_and_head_rejected(self):
        for key, value in [('run', '456'), ('attempt', '2'), ('host', 'other'), ('source', 'b' * 40)]:
            with self.subTest(key=key):
                self.reject_before_configure(lambda: self.receive(binding=dict(self.binding, **{key: value})))

    def test_modified_payload_byte_rejected(self):
        path = self.bundle / 'prefix.tar.gz'
        data = bytearray(path.read_bytes())
        data[len(data) // 2] ^= 1
        path.write_bytes(data)
        self.reject_before_configure(self.receive)

    def test_missing_artifact_rejected(self):
        (self.bundle / 'envelope.json').unlink()
        self.reject_before_configure(self.receive)

    def test_wrong_envelope_digest_rejected(self):
        self.envelope_hash = '0' * 64
        self.reject_before_configure(self.receive)

    def test_missing_payload_file_rejected(self):
        (self.bundle / 'prefix.tar.gz').unlink()
        self.reject_before_configure(self.receive)

    def test_payload_symlink_rejected(self):
        path = self.bundle / 'prefix.tar.gz'
        path.rename(self.bundle / 'saved.tar.gz')
        path.symlink_to('saved.tar.gz')
        self.reject_before_configure(self.receive)

    def test_existing_prefix_not_overwritten(self):
        self.prefix.mkdir()
        marker = self.prefix / 'preserve'
        marker.write_text('existing data')
        with self.assertRaisesRegex(dp.ContractError, 'already exists'):
            self.receive()
        self.assertEqual('existing data', marker.read_text())

    def test_archive_traversal_and_absolute_path_rejected(self):
        for name in ('prefix/../../outside', '/outside'):
            with self.subTest(name=name):
                self.replace_archive(lambda rows: setattr(rows[-1][0], 'name', name))
                self.reject_before_configure(self.receive)
        self.assertFalse((self.root / 'outside').exists())

    def test_duplicate_archive_path_rejected(self):
        self.replace_archive(lambda rows: rows.append(rows[-1]))
        self.reject_before_configure(self.receive)

    def test_hardlink_and_special_file_rejected(self):
        def hardlink(rows):
            row = next(member for member, _ in rows if member.name.endswith('tool-link'))
            row.type = tarfile.LNKTYPE
        self.replace_archive(hardlink)
        self.reject_before_configure(self.receive)
        def fifo(rows):
            rows[-1][0].type = tarfile.FIFOTYPE
        self.replace_archive(fifo)
        self.reject_before_configure(self.receive)

    def test_resealed_archive_with_wrong_content_rejected_after_extraction(self):
        def corrupt(rows):
            for i, (member, data) in enumerate(rows):
                if member.name == 'prefix/share/config.site':
                    rows[i] = (member, b'X' + data[1:])
        self.replace_archive(corrupt)
        self.reject_before_configure(self.receive)

    def test_external_link_rejected_even_with_resealed_manifest(self):
        manifest = dp.read_json(self.bundle / 'prefix-manifest.json')
        next(row for row in manifest['entries'] if row['type'] == 'symlink')['target'] = '../../../outside'
        dp.write_json(self.bundle / 'prefix-manifest.json', manifest)
        self.reseal()
        self.reject_before_configure(self.receive)

    def test_link_parent_and_link_cycle_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        next(row for row in manifest['entries'] if row['path'] == 'native')['type'] = 'symlink'
        next(row for row in manifest['entries'] if row['path'] == 'native')['target'] = '.'
        with self.assertRaises(dp.ContractError):
            dp.validate_entries(manifest['entries'], self.prefix)
        manifest = copy.deepcopy(self.manifest)
        next(row for row in manifest['entries'] if row['path'] == 'native/tool-link')['target'] = 'tool-link'
        with self.assertRaisesRegex(dp.ContractError, 'cyclic'):
            dp.validate_entries(manifest['entries'], self.prefix)

    def test_missing_hidden_stamp_rejected(self):
        rows = [row for row in self.manifest['entries'] if not row['path'].startswith('.stamp_')]
        with self.assertRaisesRegex(dp.ContractError, 'stamp'):
            dp.validate_entries(rows, self.prefix)

    def test_symlink_then_parent_is_resolved_in_filesystem_order(self):
        preserved = self.root / 'producer-preserved'
        (preserved / 'native/root-link').symlink_to('..')
        (preserved / 'native/outside').touch()
        (preserved / 'native/bad-link').symlink_to('root-link/../outside')
        with self.assertRaisesRegex(dp.ContractError, 'escapes'):
            dp.prefix_manifest(preserved, self.prefix)

    def test_duplicate_json_key_rejected(self):
        path = self.root / 'duplicate.json'
        path.write_text('{"a":1,"a":2}')
        with self.assertRaisesRegex(dp.ContractError, 'duplicate JSON'):
            dp.read_json(path)

    def test_nonfinite_json_and_boolean_schema_rejected(self):
        path = self.root / 'nonfinite.json'
        path.write_text('{"a":NaN}')
        with self.assertRaisesRegex(dp.ContractError, 'non-finite'):
            dp.read_json(path)
        manifest = dp.read_json(self.bundle / 'prefix-manifest.json')
        manifest['schema'] = True
        dp.write_json(self.bundle / 'prefix-manifest.json', manifest)
        self.reseal()
        self.reject_before_configure(self.receive)

    def test_oversize_payload_rejected_without_extracting(self):
        rows = copy.deepcopy(self.manifest['entries'])
        next(row for row in rows if row['type'] == 'file')['bytes'] = dp.MAX_PAYLOAD + 1
        with self.assertRaisesRegex(dp.ContractError, 'limit'):
            dp.validate_entries(rows, self.prefix)


class InputAndCliTest(unittest.TestCase):
    def test_actual_worktree_identity_enumerates_versioned_inputs_without_building(self):
        root = Path(__file__).resolve().parents[2]
        with patch.dict(os.environ, {name: '' for name in dp.OVERRIDES}):
            identity = dp.input_identity(root, 'ghcr.io/example/image@sha256:' + 'a' * 64,
                                         '.github/workflows/build.yml')
        self.assertGreater(len(identity['sources']), 0)
        self.assertTrue(any(row['path'] == 'depends/hosts/linux.mk' for row in identity['sources']))
        self.assertTrue(any(row['path'] == 'depends/patches/boost/process_macos_sdk.patch' for row in identity['sources']))
        self.assertEqual(['make', '-j4', '-C', 'depends', 'HOST=' + dp.HOST], identity['recipe'])
        self.assertTrue(all(value == '' for value in identity['environment'].values()))

    def test_meta_and_patch_fixtures_do_not_modify_packages(self):
        self.assertEqual('depends/hosts/linux.mk', dp.FIXTURES['C3'][0])
        self.assertIn(dp.META_MARKER.encode(), dp.changed_fixture(b'baseline\n', 'C3'))
        before = dp.FIXTURES['C4'][1].encode() + b'\n'
        self.assertIn(dp.PATCH_MARKER.encode(), dp.changed_fixture(before, 'C4'))
        with self.assertRaisesRegex(dp.ContractError, 'unique'):
            dp.changed_fixture(b'wrong patch', 'C4')

    def test_unpinned_image_and_override_fail_before_git_or_tools(self):
        with self.assertRaisesRegex(dp.ContractError, 'pinned'):
            dp.input_identity(Path.cwd(), 'ghcr.io/example/image:latest', '.github/workflows/build.yml')
        with patch.dict(os.environ, {'DEBUG': '1'}), self.assertRaisesRegex(dp.ContractError, 'override'):
            dp.input_identity(Path.cwd(), 'ghcr.io/example/image@sha256:' + 'a' * 64, '.github/workflows/build.yml')

    def test_cli_nonzero_prevents_configure_entry(self):
        # Exercise the actual CLI failure boundary, without a shell or any build.
        with tempfile.TemporaryDirectory(prefix='depends-cli-test-') as temp:
            sentinel = Path(temp) / 'configure-entered'
            wrapper = ('import pathlib,subprocess,sys; '
                       'r=subprocess.run(sys.argv[2:]); '
                       'r.check_returncode(); pathlib.Path(sys.argv[1]).touch()')
            result = subprocess.run([sys.executable, '-c', wrapper, str(sentinel), sys.executable,
                str(Path(dp.__file__).resolve()), 'receive', '--root', temp,
                '--image', 'ghcr.io/example/image:latest', '--identity', str(Path(temp) / 'missing.json'),
                '--bundle', str(Path(temp) / 'missing-bundle')], capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn('image must be pinned', result.stderr)
            self.assertFalse(sentinel.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
