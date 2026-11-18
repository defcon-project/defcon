#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The macOS app bundle: the build, the Info.plist and the deploy script agree on one name.

The deploy script runs on a disposable bundle with a stub objdump that reports no libraries, so
no Mac, Qt or SDK is needed; a real Mac run is the macOS smoke workflow's job.
"""

import os
from pathlib import Path
import platform
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'contrib' / 'macdeploy' / 'macdeployqtplus'
PLIST_IN = ROOT / 'share' / 'qt' / 'Info.plist.in'
MAKEFILE_AM = ROOT / 'Makefile.am'
GUIUTIL = ROOT / 'src' / 'qt' / 'guiutil.cpp'
STUB_OBJDUMP = '#!/bin/sh\n# the inspected binary, and no libraries\necho "$3:"\n'


def plist_from_template(**overrides):
    text = re.sub(r'@[A-Z_]+@', '1', PLIST_IN.read_text(encoding='utf-8'))
    plist = plistlib.loads(text.encode('utf-8'))
    plist.update(overrides)
    return plist


def bundle_name():
    match = re.search(r'^OSX_APP=(\S+)\.app$', MAKEFILE_AM.read_text(encoding='utf-8'), re.M)
    assert match, 'Makefile.am sets no OSX_APP'
    return match.group(1)


class SourceNamesTest(unittest.TestCase):
    def test_plist_names_the_binary_the_build_installs(self):
        name = bundle_name()
        makefile = MAKEFILE_AM.read_text(encoding='utf-8')
        self.assertIn(f'$(OSX_APP)/Contents/MacOS/{name}:', makefile)
        self.assertEqual(plist_from_template()['CFBundleExecutable'], name)

    def test_deploy_copies_the_plugins_a_static_build_imports(self):
        # A static build links these in (src/qt/bitcoin.cpp); a deployed bundle with a dynamic Qt
        # needs the same plugins as files, or the window has no native style and no logo.
        dynamic = {'QCocoaIntegrationPlugin': 'libqcocoa', 'QMacStylePlugin': 'libqmacstyle',
                   'QSvgPlugin': 'libqsvg', 'QSvgIconPlugin': 'libqsvgicon'}
        source = (ROOT / 'src' / 'qt' / 'bitcoin.cpp').read_text(encoding='utf-8')
        cocoa = source.split('#elif defined(QT_QPA_PLATFORM_COCOA)', 1)[1].split('#endif', 1)[0]
        svg = source.split('#if defined(QT_STATIC_SVG)', 1)[1].split('#endif', 1)[0]
        imported = re.findall(r'Q_IMPORT_PLUGIN\((\w+)\)', cocoa + svg)
        self.assertEqual(sorted(imported), sorted(dynamic))
        script = SCRIPT.read_text(encoding='utf-8')
        names = re.search(r"pluginName\.split\('\.'\)\[0\] not in \[([^\]]*)\]", script).group(1)
        for plugin in imported:
            self.assertIn(f"'{dynamic[plugin]}'", names)

    def test_bundle_identifiers_are_defcon_own(self):
        # On a Mac that also runs Dash Core, a shared identifier would mix the two apps' permissions,
        # notifications and "Open with" entries. The notification handler reports the identifier
        # itself, so it must name the same one as the plist.
        plist = plist_from_template()
        self.assertEqual(plist['CFBundleIdentifier'], 'io.dfcn.Defcon-Qt')
        self.assertEqual([t['CFBundleURLName'] for t in plist['CFBundleURLTypes']], ['io.dfcn.DefconPayment'])
        handler = (ROOT / 'src' / 'qt' / 'macnotificationhandler.mm').read_text(encoding='utf-8')
        self.assertEqual(re.findall(r'return @"([^"]+)";', handler), [plist['CFBundleIdentifier']])
        self.assertNotIn('org.dash', PLIST_IN.read_text(encoding='utf-8'))

    def test_plist_registers_the_uri_scheme_the_wallet_reads(self):
        match = re.search(r'uri\.scheme\(\) != QString\("([a-z]+)"\)', GUIUTIL.read_text(encoding='utf-8'))
        self.assertIsNotNone(match)
        schemes = [s for t in plist_from_template()['CFBundleURLTypes'] for s in t['CFBundleURLSchemes']]
        self.assertEqual(schemes, [match.group(1)])


@unittest.skipIf(platform.system() == 'Darwin', 'on a Mac the script signs the bundle; the smoke workflow covers that')
class DeployTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='defcon-macdeploy-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.name = bundle_name()
        self.app = self.root / f'{self.name}.app'
        (self.app / 'Contents' / 'MacOS').mkdir(parents=True)
        (self.app / 'Contents' / 'Resources').mkdir()
        (self.app / 'Contents' / 'MacOS' / self.name).write_bytes(b'not a real binary')
        self.translations = self.root / 'translations'
        self.translations.mkdir()
        (self.translations / 'qt_de.qm').write_bytes(b'qm')
        self.objdump = self.root / 'objdump'
        self.objdump.write_text(STUB_OBJDUMP, encoding='utf-8')
        self.objdump.chmod(0o755)

    def write_plist(self, **overrides):
        with open(self.app / 'Contents' / 'Info.plist', 'wb') as f:
            plistlib.dump(plist_from_template(**overrides), f)

    def deploy(self):
        env = dict(os.environ, OBJDUMP=str(self.objdump))
        env.pop('QTDIR', None)
        return subprocess.run([sys.executable, str(SCRIPT), self.app.name, 'DeFCoN-Core',
                               '-translations-dir', str(self.translations), '-zip'],
                              cwd=self.root, env=env, capture_output=True, text=True, timeout=60)

    def test_deploys_the_bundle_its_plist_names(self):
        self.write_plist()
        result = self.deploy()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        dist = self.root / 'dist' / self.app.name
        self.assertTrue((dist / 'Contents' / 'MacOS' / self.name).is_file())
        self.assertTrue((dist / 'Contents' / 'Resources' / 'qt.conf').is_file())
        self.assertTrue((dist / 'Contents' / 'Resources' / 'qt_de.qm').is_file())
        with zipfile.ZipFile(self.root / 'DeFCoN-Core.zip') as z:
            self.assertIn(f'{self.app.name}/Contents/MacOS/{self.name}', z.namelist())

    def test_refuses_a_plist_that_names_another_binary(self):
        # The state before this fix: the plist still named the binary Dash-Qt.
        self.write_plist(CFBundleExecutable='Dash-Qt')
        result = self.deploy()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('CFBundleExecutable', result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertFalse((self.root / 'DeFCoN-Core.zip').exists())

    def test_refuses_a_bundle_without_a_plist(self):
        result = self.deploy()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Could not read', result.stderr)
        self.assertNotIn('Traceback', result.stderr)

    def test_refuses_a_broken_plist_with_a_plain_error(self):
        cases = {
            'truncated XML': (b'<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>', 'Could not read'),
            'not a plist': (b'not a property list', 'Could not read'),
            # plistlib fails on these with AttributeError and IndexError, not with a parse error
            'a malformed date': (b'<?xml version="1.0"?>\n<plist version="1.0"><date>bad</date></plist>', 'Could not read'),
            'a key with no dict': (b'<?xml version="1.0"?>\n<plist version="1.0"><key>CFBundleExecutable</key></plist>', 'Could not read'),
            'array at the root': (plistlib.dumps(['Defcon-Qt']), 'names no CFBundleExecutable'),
            'number as the name': (plistlib.dumps(plist_from_template(CFBundleExecutable=7)), 'names no CFBundleExecutable'),
            'a path as the name': (plistlib.dumps(plist_from_template(CFBundleExecutable='../MacOS/Defcon-Qt')), 'names no CFBundleExecutable'),
        }
        for name, (content, message) in cases.items():
            with self.subTest(name):
                (self.app / 'Contents' / 'Info.plist').write_bytes(content)
                result = self.deploy()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(message, result.stderr)
                self.assertNotIn('Traceback', result.stderr)


if __name__ == '__main__':
    unittest.main()
