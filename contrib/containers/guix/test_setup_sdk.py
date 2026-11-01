#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise the macOS SDK helper with small disposable archives and no network."""

import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time
import unittest


HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'scripts' / 'setup-sdk'
TOP = HERE.parents[2]
NAME = 'Xcode-1.0-TEST-extracted-SDK-with-libcxx-headers'
SETTINGS = 'SDKSettings.json'
# Stands in for tar and waits to be released, before or after the work. It can also lose a file, as an
# interrupted tar would.
TAR_SHIM = r'''#!/usr/bin/env bash
hold() {
    touch "${SHIM_ENTERED}"
    while [ ! -e "${SHIM_RELEASE}" ]; do
        sleep 0.05
    done
}
if [ -n "${SHIM_HOLD_FIRST}" ]; then
    hold
fi
"${REAL_TAR}" "$@" || exit
if [ -n "${SHIM_PARTIAL}" ]; then
    find "${2}" -name SDKSettings.json -delete
fi
if [ -z "${SHIM_HOLD_FIRST}" ]; then
    hold
fi
'''


def make_archive(content):
    """An SDK archive: the expected top directory with one file."""
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w:gz') as tar:
        info = tarfile.TarInfo(f'{NAME}/{SETTINGS}')
        info.size = len(content)
        tar.addfile(info, io.BytesIO(content))
    return data.getvalue()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class SetupSdkTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='defcon-sdk-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.server = self.root / 'server'
        self.server.mkdir()
        self.sdks = self.root / 'SDKs'
        self.sources = self.root / 'sources'
        self.sdk_dir = self.sdks / NAME
        self.marker = self.sdk_dir / '.setup-sdk.sha256'
        self.cached = self.sources / f'{NAME}.tar.gz'
        self.good = make_archive(b'expected')
        # The expected file name and layout with other content, which a name check cannot tell apart.
        self.wrong = make_archive(b'wrong')

    def serve(self, archive):
        (self.server / f'{NAME}.tar.gz').write_bytes(archive)

    def cache(self, archive):
        self.sources.mkdir()
        self.cached.write_bytes(archive)

    def env(self):
        return dict(os.environ, SDK_URL=self.server.as_uri(), SDK_PATH=str(self.sdks), SDK_SOURCES=str(self.sources),
                    XCODE_VERSION='1.0', XCODE_RELEASE='TEST', XCODE_AR_HASH=sha256(self.good))

    def run_script(self, cwd=None):
        return subprocess.run(['bash', str(SCRIPT)], env=self.env(), cwd=cwd, capture_output=True, text=True, check=False)

    def start_held(self, *, partial=False, hold_first=False):
        """Start the helper in its own process group and hold it at its tar: by default after the tar has written the files."""
        shim = self.root / 'shim'
        shim.mkdir()
        (shim / 'tar').write_text(TAR_SHIM, encoding='utf-8')
        (shim / 'tar').chmod(0o755)
        self.release = self.root / 'release'
        entered = self.root / 'entered'
        env = dict(self.env(), PATH=f"{shim}{os.pathsep}{os.environ['PATH']}", REAL_TAR=shutil.which('tar'),
                   SHIM_ENTERED=str(entered), SHIM_RELEASE=str(self.release), SHIM_PARTIAL='1' if partial else '',
                   SHIM_HOLD_FIRST='1' if hold_first else '')
        process = subprocess.Popen(['bash', str(SCRIPT)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   start_new_session=True)
        self.addCleanup(self.stop, process)
        deadline = time.monotonic() + 60
        while not entered.exists():
            self.assertIsNone(process.poll(), 'the helper ended before its extraction')
            self.assertLess(time.monotonic(), deadline, 'the helper never reached its extraction')
            time.sleep(0.01)
        return process

    def stop(self, process):
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()

    def assert_extracted(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.sdk_dir / SETTINGS).read_bytes(), b'expected')
        self.assertEqual(self.marker.read_text(encoding='utf-8').strip(), sha256(self.good))

    def assert_refused(self, result, message):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)

    def test_download_is_verified_and_extracted(self):
        self.serve(self.good)
        self.assert_extracted(self.run_script())
        self.assertEqual(self.cached.read_bytes(), self.good)
        self.assertEqual([path.name for path in self.sdks.iterdir()], [NAME])

    def test_verified_directory_is_used_again(self):
        self.serve(self.good)
        self.assert_extracted(self.run_script())
        (self.server / f'{NAME}.tar.gz').unlink()
        self.cached.unlink()
        result = self.run_script()
        self.assert_extracted(result)
        self.assertEqual(result.stdout, '')

    def test_correct_cached_archive_needs_no_download(self):
        self.cache(self.good)
        result = self.run_script()
        self.assert_extracted(result)
        self.assertNotIn('Downloading', result.stdout)

    def test_wrong_cached_archive_is_refused(self):
        self.cache(self.wrong)
        self.assert_refused(self.run_script(), 'Checksum mismatch')
        self.assertFalse(self.sdk_dir.exists())
        self.assertEqual(self.cached.read_bytes(), self.wrong)

    def test_wrong_download_is_refused(self):
        self.serve(self.wrong)
        self.assert_refused(self.run_script(), 'Checksum mismatch')
        self.assertFalse(self.sdk_dir.exists())

    def test_directory_without_marker_is_refused(self):
        self.sdk_dir.mkdir(parents=True)
        (self.sdk_dir / SETTINGS).write_bytes(b'wrong')
        self.serve(self.good)
        self.assert_refused(self.run_script(), 'does not come from the expected archive')
        self.assertEqual([path.name for path in self.sdk_dir.iterdir()], [SETTINGS])
        self.assertEqual((self.sdk_dir / SETTINGS).read_bytes(), b'wrong')

    def test_directory_of_another_archive_is_refused(self):
        self.sdk_dir.mkdir(parents=True)
        self.marker.write_text(sha256(self.wrong) + '\n', encoding='utf-8')
        self.assert_refused(self.run_script(), 'does not come from the expected archive')

    def test_removed_directory_leaves_no_approval_behind(self):
        self.cache(self.good)
        self.assert_extracted(self.run_script())
        shutil.rmtree(self.sdk_dir)
        self.sdk_dir.mkdir()
        (self.sdk_dir / SETTINGS).write_bytes(b'wrong')
        self.assert_refused(self.run_script(), 'does not come from the expected archive')

    def test_failed_extraction_leaves_nothing(self):
        # The hash is right, so only the extraction can fail: the archive is cut off in the middle.
        self.good = self.good[:len(self.good) // 2]
        self.cache(self.good)
        for _ in range(2):
            result = self.run_script()
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(list(self.sdks.iterdir()), [])

    def test_interrupted_extraction_is_extracted_again(self):
        self.cache(self.good)
        self.stop(self.start_held(partial=True))
        self.assertFalse(self.sdk_dir.exists())
        self.assert_extracted(self.run_script())

    def test_interrupted_extraction_after_an_earlier_one(self):
        self.cache(self.good)
        self.assert_extracted(self.run_script())
        shutil.rmtree(self.sdk_dir)
        self.stop(self.start_held(partial=True))
        self.assertFalse(self.sdk_dir.exists())
        self.assert_extracted(self.run_script())

    def test_parallel_runs_publish_one_complete_directory(self):
        self.cache(self.good)
        held = self.start_held()
        # The first run has written its files, and they are not under the final name.
        self.assertFalse(self.sdk_dir.exists())
        self.assert_extracted(self.run_script())
        self.release.touch()
        self.assertNotEqual(held.wait(timeout=60), 0)
        self.assert_extracted(self.run_script())
        self.assertEqual([path.name for path in self.sdks.iterdir()], [NAME])

    def test_archive_swapped_after_the_check_is_not_extracted(self):
        self.cache(self.good)
        held = self.start_held(hold_first=True)
        # The check has passed and the extraction has not started: a parallel run replaces the shared archive.
        self.cached.write_bytes(self.wrong)
        self.release.touch()
        self.assertEqual(held.wait(timeout=60), 0)
        self.assert_extracted(self.run_script())

    def test_unusual_characters_in_the_paths(self):
        # A backslash changes the output of sha256sum for a named file, a colon before the first slash of a
        # relative path makes tar expect a remote host, and a leading dash reads as an option.
        self.serve(self.good)
        for name in ('sd\\ks', 'sd:ks'):
            with self.subTest(name=name):
                self.sdks = Path(name)
                self.sdk_dir = self.root / name / NAME
                self.marker = self.sdk_dir / '.setup-sdk.sha256'
                self.assert_extracted(self.run_script(cwd=self.root))
        with self.subTest(name='-sources'):
            self.sdks = Path('SDKs')
            self.sdk_dir = self.root / 'SDKs' / NAME
            self.marker = self.sdk_dir / '.setup-sdk.sha256'
            self.sources = Path('-sources')
            (self.root / self.sources).mkdir()
            (self.root / self.sources / f'{NAME}.tar.gz').write_bytes(self.good)
            self.assert_extracted(self.run_script(cwd=self.root))

    def test_defaults_match_the_documented_sdk(self):
        script = SCRIPT.read_text(encoding='utf-8')
        default = {}
        for name in ('XCODE_VERSION', 'XCODE_RELEASE', 'XCODE_AR_HASH'):
            found = re.search(rf'^{name}="\${{{name}:-([^}}]+)}}"$', script, re.M)
            self.assertIsNotNone(found, name)
            default[name] = found.group(1)
        darwin = (TOP / 'depends' / 'hosts' / 'darwin.mk').read_text(encoding='utf-8')
        self.assertIn(f"XCODE_VERSION={default['XCODE_VERSION']}\n", darwin)
        self.assertIn(f"XCODE_BUILD_ID={default['XCODE_RELEASE']}\n", darwin)
        readme = (TOP / 'contrib' / 'macdeploy' / 'README.md').read_text(encoding='utf-8')
        self.assertIn(f"`Xcode-{default['XCODE_VERSION']}-{default['XCODE_RELEASE']}-extracted-SDK-with-libcxx-headers.tar.gz`", readme)
        self.assertIn(f"The `sha256sum` should be `{default['XCODE_AR_HASH']}`", readme)


if __name__ == '__main__':
    unittest.main()
