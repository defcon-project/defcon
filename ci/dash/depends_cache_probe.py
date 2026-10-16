#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Finite cache experiment instrumentation; never dispatches workflows or builds."""

import argparse
import copy
import datetime as dt
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import uuid

import depends_prefix as dp

REPOSITORY = 'minuszka/defcon-cache-probe-20261016'
REF = 'refs/heads/cache-probe-20261016'
WORKFLOW = '.github/workflows/depends-cache-probe.yml'
BASE = 'f41d047c7a90bd94e9c760c92dc8588a2a77d478'


def env(name):
    value = os.environ.get(name, '')
    dp.require(value, 'missing probe field: ' + name)
    return value


def output(values):
    path = os.environ.get('GITHUB_OUTPUT')
    if path:
        with Path(path).open('a') as stream:
            for key, value in values.items():
                dp.require('\n' not in str(value), 'invalid output value')
                stream.write(str(key) + '=' + str(value) + '\n')


def guard():
    dp.require(env('GITHUB_REPOSITORY') == REPOSITORY, 'probe repository not isolated')
    dp.require(env('GITHUB_REF') == REF, 'wrong probe cache-access ref')
    dp.require(env('GITHUB_EVENT_NAME') == 'workflow_dispatch', 'manual dispatch required')
    dp.require(env('GITHUB_RUN_ATTEMPT') == '1', 'reruns are outside this finite campaign')
    for name in ('BASELINE_NAMESPACE', 'FIXED_NAMESPACE'):
        value = env(name)
        parsed = uuid.UUID(value)
        dp.require(str(parsed) == value and parsed.version == 4, 'fresh UUIDv4 namespace required')
    dp.require(env('BASELINE_NAMESPACE') != env('FIXED_NAMESPACE'), 'namespaces must be independent')
    dp.require(re.fullmatch(r'ghcr\.io/[a-z0-9_./-]+@sha256:[0-9a-f]{64}', env('CI_IMAGE')), 'unpinned probe image')
    dp.require(re.fullmatch(r'[0-9a-f]{64}', env('REVIEW_PACKET_SHA256')), 'missing exact review packet digest')
    started = os.environ.get('CAMPAIGN_STARTED', '')
    if started:
        dp.require(0 <= time.time() - float(started) < 24 * 3600, 'campaign wall-clock limit reached')


def cache_inventory():
    # Read-only API. Do not log token, headers, arbitrary environment, or request objects.
    request = Request('https://api.github.com/repos/' + REPOSITORY + '/actions/caches?' + urlencode({'per_page': 100}),
                      headers={'Authorization': 'Bearer ' + env('GH_TOKEN'),
                               'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'})
    with urlopen(request, timeout=30) as response:
        data = json.load(response)
    rows = data['actions_caches']
    dp.require(data['total_count'] == len(rows) <= 6, 'cache entry limit or incomplete API inventory')
    dp.require(sum(row['size_in_bytes'] for row in rows) <= 6 * dp.MAX_ARCHIVE, 'aggregate cache size limit')
    for row in rows:
        dp.require(row['ref'] == REF and row['size_in_bytes'] <= dp.MAX_ARCHIVE, 'unexpected cache ref or size')
        allowed = ('dfcn-bcache-' + env('BASELINE_NAMESPACE') + '-baseline-',
                   'dfcn-bcache-' + env('FIXED_NAMESPACE') + '-fixed-')
        dp.require(row['key'].startswith(allowed), 'cache outside this campaign')
    return [{key: row[key] for key in ('id', 'key', 'version', 'ref', 'size_in_bytes', 'created_at')} for row in rows]


def find_cache(rows, key):
    found = [row for row in rows if row['key'] == key]
    dp.require(len(found) == 1, 'missing or ambiguous cache entry: ' + key)
    return found[0]


def prefix_state(prefix):
    if not prefix.exists():
        return {'present': False, 'prefix_sha256': None}
    manifest = dp.prefix_manifest(prefix)
    site = (prefix / 'share/config.site').read_text()
    header = prefix / 'include/boost/process/detail/posix/handles.hpp'
    return {'present': True, 'prefix_sha256': dp.digest(dp.canonical(manifest)), 'manifest': manifest,
            'meta_marker': dp.META_MARKER in site,
            'patch_marker': header.is_file() and dp.PATCH_MARKER in header.read_text()}


def classify(case, variant, producer, consumer, anchor_digest):
    dp.require(producer['present'] and consumer['present'], 'missing measured prefix')
    p, c = producer['prefix_sha256'], consumer['prefix_sha256']
    expected_meta, expected_patch = case == 'C3', case == 'C4'
    dp.require((producer['meta_marker'], producer['patch_marker']) == (expected_meta, expected_patch), 'producer marker does not prove the selected fixture')
    if case in ('C1', 'C2'):
        dp.require(p == c and not consumer['meta_marker'] and not consumer['patch_marker'], 'unchanged transfer mismatch')
        if case == 'C2':
            dp.require(c == anchor_digest, 'unchanged exact-hit is not A')
        return {'harness_result': 'SUCCESS', 'dependency_contract': 'PASS_PREFIX_ONLY'}
    dp.require(p != anchor_digest, 'changed input did not change the producer prefix')
    if variant == 'baseline':
        dp.require(c == anchor_digest and p != c, 'baseline stale prefix not reproduced')
        dp.require(not consumer['meta_marker'] and not consumer['patch_marker'], 'baseline consumer not original A')
        return {'harness_result': 'SUCCESS_REPRODUCTION', 'dependency_contract': 'FAIL_STALE_PREFIX'}
    dp.require(p == c and (consumer['meta_marker'], consumer['patch_marker']) == (expected_meta, expected_patch), 'fixed prefix is not the current producer output')
    return {'harness_result': 'SUCCESS', 'dependency_contract': 'PASS_PREFIX_ONLY'}


def compact_result(record):
    return {key: record[key] for key in ('case', 'variant', 'harness_result', 'dependency_contract',
            'post_producer_saved_cache', 'controls_rejected', 'input_sha256', 'binding')} | {
                'prefix_sha256': record['prefix_after']['prefix_sha256']}


def control_checks(bundle, prefix, identity, run_binding, envelope_hash):
    dp.require(not prefix.exists(), 'controls must precede prefix installation')
    results = []
    def reject(label, function, expected_error):
        try:
            function()
        except dp.ContractError as error:
            dp.require(expected_error in str(error), 'negative control failed for the wrong reason: ' + label)
        else:
            raise dp.ContractError('negative control was accepted: ' + label)
        dp.require(not prefix.exists(), 'negative control installed a prefix')
        results.append(label)
    wrong_input = copy.deepcopy(identity)
    wrong_input['image'] += '-wrong'
    reject('wrong-input', lambda: dp.receive(bundle, prefix, wrong_input, run_binding, envelope_hash), 'consumer input mismatch')
    wrong_binding = copy.deepcopy(run_binding)
    wrong_binding['context']['GITHUB_RUN_ATTEMPT'] = '2'
    reject('wrong-attempt', lambda: dp.receive(bundle, prefix, identity, wrong_binding, envelope_hash), 'binding mismatch')
    with tempfile.TemporaryDirectory(prefix='depends-probe-control-') as temp:
        reject('missing-artifact', lambda: dp.receive(Path(temp) / 'missing', prefix, identity, run_binding, envelope_hash), 'missing regular JSON')
        damaged = Path(temp) / 'damaged'
        shutil.copytree(bundle, damaged)
        path = damaged / 'prefix.tar.gz'
        with path.open('r+b') as stream:
            stream.seek(path.stat().st_size // 2)
            byte = stream.read(1)
            stream.seek(-1, 1)
            stream.write(bytes([byte[0] ^ 1]))
        reject('modified-payload-byte', lambda: dp.receive(damaged, prefix, identity, run_binding, envelope_hash), 'payload digest mismatch')
    return results


def save_evidence(directory, name, value):
    directory.mkdir(parents=True, exist_ok=True)
    dp.write_json(directory / name, value)


def evidence_limit(directory):
    total = sum(p.stat().st_size for p in directory.rglob('*') if p.is_file())
    dp.require(total <= 10 * 1024 ** 2, 'evidence artifact exceeds 10 MiB')


def size_check(root):
    def size(paths):
        return sum(p.stat().st_size for path in paths if path.exists()
                   for p in path.rglob('*') if p.is_file() and not p.is_symlink())
    dp.require(size([root / 'depends/sources']) <= dp.MAX_ARCHIVE, 'source-cache size limit exceeded')
    dp.require(size([root / 'depends/built', root / 'depends' / dp.HOST]) <= dp.MAX_ARCHIVE, 'binary-cache size limit exceeded')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('preflight', 'prepare', 'restored', 'produced', 'consume', 'remaining', 'summary', 'self-test'))
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--producer-evidence', type=Path)
    parser.add_argument('--bundle', type=Path)
    args = parser.parse_args()
    if args.operation == 'self-test':
        self_test()
        return
    guard()
    root, evidence = args.root.resolve(), args.evidence
    if args.operation == 'preflight':
        patch_bytes = subprocess.check_output(['git', '--no-optional-locks', '-C', str(root),
                                              'diff', '--binary', '--full-index', BASE, 'HEAD'])
        dp.require(dp.digest(patch_bytes) == env('REVIEW_PACKET_SHA256'), 'published code differs from the reviewed patch digest')
        dp.require(cache_inventory() == [], 'probe repository must start without any cache entries')
        started = str(time.time())
        output({'started': started})
        save_evidence(evidence, 'preflight.json', {'status': 'READY', 'started': started,
            'review_packet_sha256': env('REVIEW_PACKET_SHA256'), 'ref': REF,
            'image': env('CI_IMAGE'), 'cache_entries': 0,
            'limits': '8 producers at 90m, 8 consumers at 15m, 2 small jobs at 10m; no reruns'})
        return
    if args.operation == 'remaining':
        print(max(1, min(5400, int(24 * 3600 - (time.time() - float(env('CAMPAIGN_STARTED')))))))
        return
    if args.operation == 'summary':
        records = [dp.read_json(path) for path in evidence.rglob('consumer.json')]
        expected = {(variant, case) for variant in ('baseline', 'fixed') for case in ('C1', 'C2', 'C3', 'C4')}
        seen = {(row['variant'], row['case']) for row in records}
        needs = json.loads(env('NEEDS_JSON'))
        complete = seen == expected and len(records) == 8 and all(row['result'] == 'success' for row in needs.values())
        save_evidence(evidence, 'summary.json', {'status': 'EXPERIMENT_COMPLETE' if complete else 'INCONCLUSIVE',
            'cases': [compact_result(record) for record in records],
            'terminal_jobs': {key: value['result'] for key, value in needs.items()},
            'finding': 'F-D05-CI-CACHE-01', 'finding_status': 'open',
            'scope': 'Real depends and prefix transport only; no Core configure/build or full CI',
            'baseline_contract': 'FAIL_STALE_PREFIX' if complete else 'UNPROVEN',
            'fixed_contract': 'PASS_PREFIX_ONLY' if complete else 'UNPROVEN'})
        dp.require((evidence / 'summary.json').stat().st_size <= 10 * 1024 ** 2, 'summary exceeds evidence size limit')
        dp.require(complete, 'experiment evidence or terminal jobs incomplete')
        return
    case, variant = env('PROBE_CASE'), env('PROBE_VARIANT')
    dp.require(case in ('C1', 'C2', 'C3', 'C4') and variant in ('baseline', 'fixed'), 'invalid case or variant')
    prefix = root / 'depends' / dp.HOST
    identity_path = evidence / 'input.json'
    if args.operation == 'prepare':
        dp.require(not prefix.exists() and not (root / 'depends/built').exists()
                   and not (root / 'depends/sources').exists(), 'job did not start with a fresh workspace')
        if case in dp.FIXTURES:
            name = dp.FIXTURES[case][0]
            original = subprocess_blob(root, name)
            dp.require((root / name).read_bytes() == original, 'fixture was already modified')
            (root / name).write_bytes(dp.changed_fixture(original, case))
        identity = dp.input_identity(root, env('CI_IMAGE'), WORKFLOW, case)
        save_evidence(evidence, 'input.json', identity)
        input_hash = dp.digest(dp.canonical(identity))
        package_hash = dp.digest(b''.join(bytes.fromhex(row['sha256']) for row in identity['sources']
            if package_input(row['path'])))
        dp.require(package_hash == env('PACKAGE_HASH'), 'offline package projection differs from actual Actions hashFiles')
        namespace = 'dfcn-bcache-' + env('BASELINE_NAMESPACE' if variant == 'baseline' else 'FIXED_NAMESPACE') + '-' + variant
        binary = namespace + ('-Linux-depends-linux64-' + package_hash if variant == 'baseline'
                              else '-depends-v2-' + dp.HOST + '-' + input_hash)
        output({'input_sha256': input_hash, 'binary_key': binary, 'source_key': namespace + '-sources-' + package_hash})
        return
    identity = dp.input_identity(root, env('CI_IMAGE'), WORKFLOW, case)
    dp.require(dp.read_json(identity_path) == identity, 'probe inputs changed since prepare')
    run_binding = dp.binding(root, identity, env('PRODUCER_JOB'))
    if args.operation == 'restored':
        state = prefix_state(prefix)
        rows = cache_inventory()
        key = env('BINARY_KEY')
        if case == 'C1':
            dp.require(not state['present'] and env('CACHE_RESULT') == 'MISS', 'C1 was not cold')
            dp.require(not any(row['key'] == key for row in rows), 'C1 key already exists')
            matched = None
        else:
            anchor = find_cache(rows, env('A_KEY'))
            dp.require(str(anchor['id']) == env('A_ID') and anchor['version'] == env('A_VERSION'), 'A cache was evicted or changed')
            dp.require(state['prefix_sha256'] == env('A_PREFIX_SHA256'), 'producer did not restore A')
            matched = anchor
            dp.require(env('CACHE_RESULT') == ('EXACT' if case == 'C2' or variant == 'baseline' else 'FALLBACK'), 'unexpected restore classification')
        save_evidence(evidence, 'restore.json', {'cache_key': key, 'matched_cache': matched,
            'cache_result': env('CACHE_RESULT'), 'prefix_before': state, 'cache_inventory': rows})
    elif args.operation == 'produced':
        size_check(root)
        state = prefix_state(prefix)
        dp.require(state['present'], 'make produced no prefix')
        save_evidence(evidence, 'producer.json', {'case': case, 'variant': variant,
            'identity': identity, 'binding': run_binding, 'prefix_after': state,
            'make_exit': 0, 'measured_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'built_archives': [{'path': p.relative_to(root).as_posix(), 'bytes': p.stat().st_size,
                'sha256': dp.file_digest(p)} for p in sorted((root / 'depends/built').rglob('*.tar.gz'))]})
    elif args.operation == 'consume':
        dp.require(args.producer_evidence is not None, 'missing producer evidence directory')
        producer = dp.read_json(args.producer_evidence / 'producer.json')
        dp.require(producer['identity'] == identity and producer['binding'] == run_binding, 'independent consumer identity or binding mismatch')
        rows = cache_inventory()
        saved = find_cache(rows, env('BINARY_KEY'))
        if case != 'C1':
            anchor = find_cache(rows, env('A_KEY'))
            dp.require(str(anchor['id']) == env('A_ID') and anchor['version'] == env('A_VERSION'), 'saved A cache changed')
        controls = []
        if variant == 'fixed':
            dp.require(args.bundle is not None, 'missing fixed producer bundle')
            controls = control_checks(args.bundle, prefix, identity, run_binding, env('ENVELOPE_SHA256'))
            dp.receive(args.bundle, prefix, identity, run_binding, env('ENVELOPE_SHA256'))
        else:
            dp.require(env('CACHE_RESULT') == 'EXACT', 'baseline consumer did not restore the exact key')
        state = prefix_state(prefix)
        anchor_digest = state['prefix_sha256'] if case == 'C1' else env('A_PREFIX_SHA256')
        result = classify(case, variant, producer['prefix_after'], state, anchor_digest)
        record = {'case': case, 'variant': variant, **result, 'prefix_after': state,
                  'post_producer_saved_cache': saved, 'controls_rejected': controls,
                  'input_sha256': dp.digest(dp.canonical(identity)), 'binding': run_binding}
        save_evidence(evidence, 'consumer.json', record)
        output({'cache_id': str(saved['id']), 'cache_version': saved['version'],
                'prefix_sha256': state['prefix_sha256']})
    evidence_limit(evidence)


def subprocess_blob(root, name):
    return subprocess.check_output(['git', '--no-optional-locks', '-C', str(root), 'show', 'HEAD:' + name])


def package_input(path):
    return path.startswith('depends/packages/') and path.count('/') == 2


class ProbeOracleTest(unittest.TestCase):
    @staticmethod
    def state(value, meta=False, patch=False):
        return {'present': True, 'prefix_sha256': value, 'meta_marker': meta, 'patch_marker': patch}

    def test_baseline_stale_is_contract_failure_and_reproduction_success(self):
        result = classify('C3', 'baseline', self.state('B', meta=True), self.state('A'), 'A')
        self.assertEqual('SUCCESS_REPRODUCTION', result['harness_result'])
        self.assertEqual('FAIL_STALE_PREFIX', result['dependency_contract'])

    def test_fixed_meta_and_independent_patch_cases(self):
        for case, meta, patch_marker in [('C3', True, False), ('C4', False, True)]:
            with self.subTest(case=case):
                value = self.state('B', meta, patch_marker)
                self.assertEqual('PASS_PREFIX_ONLY', classify(case, 'fixed', value, value, 'A')['dependency_contract'])

    def test_wrong_anchor_missing_marker_and_contaminated_C4_not_accepted(self):
        bad_cases = [
            ('C3', 'baseline', self.state('B', meta=True), self.state('C'), 'A'),
            ('C3', 'fixed', self.state('B'), self.state('B'), 'A'),
            ('C4', 'fixed', self.state('B', meta=True, patch=True), self.state('B', patch=True), 'A'),
            ('C2', 'fixed', self.state('B'), self.state('B'), 'A'),
        ]
        for args in bad_cases:
            with self.subTest(case=args[0], variant=args[1]):
                with self.assertRaises(dp.ContractError):
                    classify(*args)

    def test_summary_keeps_evidence_digests_without_copying_full_manifests(self):
        record = dict(case='C1', variant='fixed', harness_result='SUCCESS',
            dependency_contract='PASS_PREFIX_ONLY', post_producer_saved_cache={'id': 1},
            controls_rejected=[], input_sha256='a' * 64, binding={},
            prefix_after={'prefix_sha256': 'b' * 64, 'manifest': {'large': 'not copied'}})
        compact = compact_result(record)
        self.assertNotIn('prefix_after', compact)
        self.assertEqual('b' * 64, compact['prefix_sha256'])


def self_test():
    from test_depends_prefix import PrefixTransportTest
    class ProbeControlsTest(PrefixTransportTest):
        def test_probe_controls_use_real_bundle_bytes_and_preserve_positive_payload(self):
            before = dp.file_digest(self.bundle / 'prefix.tar.gz')
            checks = control_checks(self.bundle, self.prefix, self.identity, self.binding, self.envelope_hash)
            self.assertEqual(['wrong-input', 'wrong-attempt', 'missing-artifact', 'modified-payload-byte'], checks)
            self.assertEqual(before, dp.file_digest(self.bundle / 'prefix.tar.gz'))
            self.receive()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProbeOracleTest)
    suite.addTest(ProbeControlsTest('test_probe_controls_use_real_bundle_bytes_and_preserve_positive_payload'))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    dp.require(result.wasSuccessful(), 'diagnostic self-tests failed')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Missing cache/API/terminal evidence is inconclusive, never a green fallback.
        print('cache experiment INCONCLUSIVE: ' + str(error), file=sys.stderr)
        sys.exit(1)
