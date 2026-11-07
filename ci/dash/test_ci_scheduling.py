#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""No-build guards for parallel CI lanes, cache scope and the required check."""

import contextlib
import copy
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/build.yml'
sys.path.insert(0, str(ROOT / 'ci/dash'))
import functional_gate  # noqa: E402


def job_text(workflow, name):
    match = re.search(r'^  ' + re.escape(name) + r':\n(.*?)(?=^  [\w-]+:|\Z)',
                      workflow, re.MULTILINE | re.DOTALL)
    if not match:
        raise AssertionError('Missing job: ' + name)
    return match.group(1)


def step_text(job, name):
    match = re.search(r'^      - name: ' + re.escape(name) + r'\n(.*?)(?=^      - name:|\Z)',
                      job, re.MULTILINE | re.DOTALL)
    if not match:
        raise AssertionError('Missing step: ' + name)
    return match.group(1)


def run_script(step):
    match = re.search(r'^        run: \|\n((?:          [^\n]*\n|\n)*)', step, re.MULTILINE)
    if not match:
        raise AssertionError('Missing multiline run block')
    return textwrap.dedent(match.group(1))


class SchedulingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = WORKFLOW.read_text()
        cls.checks = job_text(cls.workflow, 'checks')
        cls.build = job_text(cls.workflow, 'build')
        cls.gate = run_script(step_text(cls.build, 'Require every CI lane'))

    def successful_needs(self, code='true'):
        needs = {name: {'result': 'success', 'outputs': {}}
                 for name in ('changes', 'build-image', 'build-depends', 'checks')}
        needs['changes']['outputs']['code'] = code
        return needs

    def run_gate(self, needs):
        env = dict(os.environ, NEEDS_JSON=json.dumps(needs), PYTHONDONTWRITEBYTECODE='1')
        return subprocess.run(['bash', '-eo', 'pipefail', '-c', self.gate], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def test_required_check_keeps_its_exact_name_and_waits_even_on_failure(self):
        self.assertIn('    name: Build (linux64, x86_64-pc-linux-gnu, linux64)\n', self.build)
        self.assertIn('    if: ${{ always() }}\n', self.build)
        self.assertIn('    needs: [changes, build-image, build-depends, checks]\n', self.build)
        self.assertIn('          NEEDS_JSON: ${{ toJSON(needs) }}\n', self.build)

    def test_code_and_documentation_success(self):
        for code in ('true', 'false'):
            with self.subTest(code=code):
                result = self.run_gate(self.successful_needs(code))
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertIn('All required CI lanes succeeded', result.stdout)

    def test_every_failed_cancelled_or_skipped_prerequisite_blocks_the_gate(self):
        for code in ('true', 'false'):
            for name in self.successful_needs():
                for status in ('failure', 'cancelled', 'skipped', ''):
                    with self.subTest(code=code, job=name, status=status):
                        needs = self.successful_needs(code)
                        needs[name]['result'] = status
                        result = self.run_gate(needs)
                        self.assertNotEqual(0, result.returncode)
                        self.assertIn('CI prerequisites did not succeed', result.stderr)

    def test_missing_or_extra_prerequisite_is_not_a_success(self):
        for name in self.successful_needs():
            with self.subTest(missing=name):
                needs = self.successful_needs()
                del needs[name]
                self.assertNotEqual(0, self.run_gate(needs).returncode)
        needs = self.successful_needs()
        needs['unexpected'] = copy.deepcopy(needs['checks'])
        self.assertNotEqual(0, self.run_gate(needs).returncode)

    def test_missing_or_invalid_change_decision_blocks_the_gate(self):
        for code in ('', 'TRUE', 'False', 'unknown', True, False, None):
            with self.subTest(code=code):
                self.assertNotEqual(0, self.run_gate(self.successful_needs(code)).returncode)
        needs = self.successful_needs()
        del needs['changes']['outputs']['code']
        self.assertNotEqual(0, self.run_gate(needs).returncode)

    def test_malformed_needs_do_not_turn_green(self):
        for value in (None, [], {}, {'checks': 'success'}):
            with self.subTest(needs=value):
                self.assertNotEqual(0, self.run_gate(value).returncode)
        needs = self.successful_needs()
        del needs['checks']['result']
        self.assertNotEqual(0, self.run_gate(needs).returncode)

    def test_lanes_are_independent_and_do_not_cancel_each_other(self):
        self.assertIn('    needs: [changes, build-image, build-depends]\n', self.checks)
        self.assertIn('      fail-fast: false\n', self.checks)
        self.assertEqual(['runtime', 'functional', 'build-options'],
                         re.findall(r'^            lane: (.+)$', self.checks, re.MULTILINE))

    def test_each_product_gate_has_one_lane_and_is_not_removed(self):
        lanes = {
            'runtime': ('Unit suites (all but the named exceptions)', 'Qt tests and leak check',
                        'Upload build artifacts'),
            'functional': ('Functional tests (named gate)',),
            'build-options': ('Prepare the build-options source', 'Configure option regressions',
                              'No-wallet build and RPC/PoS regressions'),
        }
        for lane, names in lanes.items():
            for name in names:
                with self.subTest(step=name):
                    condition = "        if: needs.changes.outputs.code == 'true' && matrix.lane == '" + lane + "'\n"
                    self.assertIn(condition, step_text(self.checks, name))

    def test_both_test_lanes_build_the_same_tree_the_same_way(self):
        # The functional lane runs what the runtime lane built before the split: the same script, the same
        # configuration, so the same compiler cache entries.
        build = step_text(self.checks, 'Build source and run tests')
        self.assertIn("        if: needs.changes.outputs.code == 'true' && "
                      "(matrix.lane == 'runtime' || matrix.lane == 'functional')\n", build)
        self.assertIn('          ./ci/dash/build_src.sh\n', build)
        self.assertNotIn('matrix.lane', run_script(build))

    def test_options_lane_prepares_autotools_without_running_the_main_build(self):
        step = step_text(self.checks, 'Prepare the build-options source')
        self.assertIn('        run: ./autogen.sh\n', step)
        self.assertLess(self.checks.index('name: Prepare the build-options source'),
                        self.checks.index('name: Configure option regressions'))

    def test_all_shells_share_the_persisted_cache_directory(self):
        self.assertIn('    env:\n      CACHE_DIR: /cache\n      CCACHE_DIR: /cache/ccache\n      CCACHE_SIZE: 400M\n',
                      self.checks)
        build = run_script(step_text(self.checks, 'Build source and run tests'))
        self.assertNotRegex(build, r'(?m)^\s*(?:export )?(?:CACHE_DIR|CCACHE_DIR|CCACHE_SIZE)=')
        prepare = run_script(step_text(self.checks, 'Prepare the compiler cache'))
        self.assertIn('ccache --zero-stats --max-size="$CCACHE_SIZE"', prepare)

    def test_parallel_cache_writers_have_distinct_input_bound_keys(self):
        cache = step_text(self.checks, 'CCache')
        keys = re.findall(r'^          key: (.+)$', cache, re.MULTILINE)
        self.assertEqual(1, len(keys))
        for field in ('matrix.lane', 'steps.identity.outputs.input_sha256', 'github.sha'):
            self.assertIn('${{ ' + field + ' }}', keys[0])
        self.assertIn('            /cache\n', cache)
        self.assertIn("matrix.lane == 'runtime' && format", cache)
        self.assertIn('        id: identity\n', step_text(self.checks, 'Identify consumer inputs independently'))

    def test_depends_cache_falls_back_only_to_the_same_depends_tree_and_image(self):
        depends = job_text(self.workflow, 'build-depends')
        reuse = step_text(depends, 'Identify the depends tree and image for cache reuse')
        self.assertIn("        if: needs.changes.outputs.code == 'true'\n", reuse)
        self.assertIn('        id: reuse\n', reuse)
        self.assertIn('          IMAGE_REF: ${{ needs.build-image.outputs.image-ref }}\n', reuse)
        cache = step_text(depends, 'Cache depends')
        self.assertEqual(['depends-v3-${{ runner.os }}-${{ matrix.host }}-${{ steps.reuse.outputs.key }}-'
                          '${{ steps.identity.outputs.input_sha256 }}'],
                         re.findall(r'^          key: (.+)$', cache, re.MULTILINE))
        fallbacks = re.search(r'^          restore-keys: \|\n((?:            [^\n]*\n)+)', cache, re.MULTILINE).group(1)
        self.assertEqual(['depends-v3-${{ runner.os }}-${{ matrix.host }}-${{ steps.reuse.outputs.key }}-'],
                         [line.strip() for line in fallbacks.splitlines()])
        self.assertLess(depends.index('      - name: Identify dependency inputs before restore\n'),
                        depends.index('      - name: Identify the depends tree and image for cache reuse\n'))
        self.assertLess(depends.index('      - name: Identify the depends tree and image for cache reuse\n'),
                        depends.index('      - name: Cache depends\n'))

        # The reuse key changes with the depends tree or the image and with nothing else; no image digest, no key.
        script = run_script(reuse)
        with tempfile.TemporaryDirectory() as tmp:
            def git(*args):
                subprocess.run(['git', '-C', tmp, '-c', 'user.name=t', '-c', 'user.email=t@t',
                                '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=' + os.devnull, *args],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            def key(image):
                out = Path(tmp) / 'out'
                out.write_text('')
                env = dict(os.environ, IMAGE_REF=image, GITHUB_OUTPUT=str(out))
                result = subprocess.run(['bash', '-c', script], cwd=tmp, env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                return result.returncode, out.read_text()

            git('init', '-q')
            (Path(tmp) / 'depends').mkdir()
            (Path(tmp) / 'depends/Makefile').write_text('one\n')
            (Path(tmp) / 'build.yml').write_text('workflow one\n')
            git('add', '.')
            git('commit', '-q', '-m', 'one')
            image = 'ghcr.io/x/runner@sha256:' + 'a' * 64
            status, first = key(image)
            self.assertEqual(0, status)
            self.assertRegex(first, r'^key=[0-9a-f]{32}\n$')
            (Path(tmp) / 'build.yml').write_text('workflow two\n')
            git('commit', '-q', '-am', 'workflow')
            self.assertEqual((0, first), key(image))
            self.assertNotEqual(first, key('ghcr.io/x/runner@sha256:' + 'b' * 64)[1])
            (Path(tmp) / 'depends/Makefile').write_text('two\n')
            git('commit', '-q', '-am', 'depends')
            self.assertNotEqual(first, key(image)[1])
            for bad in ('', 'ghcr.io/x/runner:latest', 'ghcr.io/x/runner@sha256:' + 'a' * 63):
                with self.subTest(image=bad):
                    self.assertNotEqual(0, key(bad)[0])

    def test_compiler_cache_falls_back_across_identities_but_never_across_lanes(self):
        # ccache finds objects by compiler, arguments and preprocessed source, so an older identity's cache is
        # at worst a set of misses. The fallback keeps the lane: no-wallet objects never fill the runtime cache.
        fallbacks = re.search(r'^          restore-keys: \|\n((?:            [^\n]*\n)+)',
                              step_text(self.checks, 'CCache'), re.MULTILINE).group(1)
        lines = [line.strip() for line in fallbacks.splitlines()]
        self.assertEqual(4, len(lines))
        self.assertEqual('ccache-v2-${{ runner.os }}-${{ matrix.build_target }}-${{ matrix.lane }}-', lines[2])
        for line in lines[:3]:
            self.assertTrue(line.startswith('ccache-v2-${{ runner.os }}-${{ matrix.build_target }}-${{ matrix.lane }}-'))

    def test_functional_lane_reads_the_runtime_cache_and_saves_none(self):
        self.assertIn("        if: needs.changes.outputs.code == 'true' && matrix.lane != 'functional'\n",
                      step_text(self.checks, 'CCache'))
        restore = step_text(self.checks, "Restore the runtime lane's compiler cache")
        self.assertIn("        if: needs.changes.outputs.code == 'true' && matrix.lane == 'functional'\n", restore)
        self.assertIn('        uses: actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830 # v4\n', restore)
        self.assertIn('            /cache\n', restore)
        keys = re.findall(r'^          key: (.+)$', restore, re.MULTILINE)
        self.assertEqual(['ccache-v2-${{ runner.os }}-${{ matrix.build_target }}-runtime-'
                          '${{ steps.identity.outputs.input_sha256 }}-${{ github.sha }}'], keys)
        fallbacks = re.search(r'^          restore-keys: \|\n((?:            [^\n]*\n)+)', restore, re.MULTILINE).group(1)
        self.assertEqual(['ccache-v2-${{ runner.os }}-${{ matrix.build_target }}-runtime-'
                          '${{ steps.identity.outputs.input_sha256 }}-${{ steps.vars.outputs.PR_BASE_SHA }}',
                          'ccache-v2-${{ runner.os }}-${{ matrix.build_target }}-runtime-'
                          '${{ steps.identity.outputs.input_sha256 }}-',
                          'ccache-v2-${{ runner.os }}-${{ matrix.build_target }}-runtime-'],
                         [line.strip() for line in fallbacks.splitlines()])
        # The one step in the lanes that saves a compiler cache is the CCache step, which skips this lane.
        self.assertEqual(1, self.checks.count('uses: actions/cache@'))
        self.assertEqual(1, self.checks.count('uses: actions/cache/restore@'))
        self.assertNotIn('actions/cache/save', self.workflow)

    def test_no_wallet_build_path_is_stable_but_fresh(self):
        build = run_script(step_text(self.checks, 'No-wallet build and RPC/PoS regressions'))
        self.assertIn('nowallet_build="$RUNNER_TEMP/no-wallet"\n', build)
        self.assertIn('mkdir "$nowallet_build"\n', build)
        self.assertNotIn('mkdir -p "$nowallet_build"', build)
        self.assertNotIn('rm -rf', build)
        self.assertIn('--disable-wallet', build)

    def test_cache_statistics_are_reported_even_after_a_failed_lane(self):
        report = step_text(self.checks, 'Report compiler cache statistics')
        self.assertIn("if: ${{ always() && needs.changes.outputs.code == 'true' }}", report)
        self.assertIn('ccache --show-stats', report)
        self.assertIn('tee -a "$GITHUB_STEP_SUMMARY"', report)

    def test_original_unit_selection_is_preserved(self):
        unit = run_script(step_text(self.checks, 'Unit suites (all but the named exceptions)'))
        selection = re.search(r"--run_test='([^']+)'", unit).group(1)
        self.assertEqual('!bip324_tests:!bip39_tests:!blockencodings_tests:!denialofservice_tests:'
                         '!descriptor_tests:!dynamic_activation_thresholds_tests:!evo_utils_tests:'
                         '!governance_validators_tests:!key_io_tests:!miner_tests/CreateNewBlock_validity:'
                         '!rpc_tests:!transaction_tests:!util_tests:!validation_chainstate_tests:'
                         '!wallet_tests', selection)

    def test_original_functional_gate_tests_and_runner_flags_are_preserved(self):
        # Dropping a test from the gate takes an edit here as well as a line in the list that says why.
        self.assertEqual(sorted([
            'feature_llmq_chainlocks.py', 'feature_pos_staking.py', 'feature_pos_stake_combine.py',
            'feature_pos_verifychain.py', 'feature_pos_header_only.py',
            'p2p_unbacked_pos_headers.py',
            'feature_dsl_sign_wait.py', 'feature_dsl_commitment_v2.py',
            'feature_addressindex_coinstake.py', 'feature_addressindex.py', 'feature_addressindex_type_boundary.py',
            'p2p_instantsend.py', 'rpc_verifyislock.py', 'rpc_bls.py',
            'feature_pos_mixingonly_unlock.py',
            'feature_dsl_service.py', 'feature_dsl_perf.py', 'feature_dsl_perf_burst.py',
            'feature_llmq_is_nonrotation.py', 'feature_llmq_is_nonrotated_verify.py',
            'feature_llmq_is_named_quorum_window.py', 'feature_llmq_no_rotation.py', 'feature_superblock_retirement.py',
            'feature_v23_bundle_heights.py', 'feature_v231_no_payee.py', 'feature_llmq_q60_proto_floor.py',
            'feature_evodb_reconcile.py',
            'rpc_verifychainlock.py', 'feature_evodb_reconcile_below_dip3.py', 'feature_evodb_repair_startup.py',
            'feature_llmq_q60_regtest.py', 'feature_llmq_switchover_config.py', 'p2p_headers_uncompressed.py',
            'feature_dsl_commit_decision_log.py',
            'feature_pos_double_sign.py',
        ]), sorted(name for name, _ in functional_gate.read_gate()))
        functional = run_script(step_text(self.checks, 'Functional tests (named gate)'))
        self.assertIn('python3 ci/dash/functional_gate.py order > "$gate" || exit 1\n', functional)
        self.assertIn('mapfile -t GATE < "$gate"\n', functional)
        self.assertIn('python3 test/functional/test_runner.py --jobs=4 --timeout-factor="${TEST_RUNNER_TIMEOUT_FACTOR}" '
                      '--combinedlogslen=4000 --tmpdirprefix="$RUNNER_TEMP/functional-tests" "${GATE[@]}" 2>&1 '
                      '| tee "$log" || status=$?\n', functional)
        self.assertNotIn('--failfast', functional)
        self.assertNotIn('--ci', functional)
        # The duration report runs after the runner and never decides the step: the runner's status does.
        self.assertTrue(functional.rstrip().endswith('exit "$status"'))
        report = [line for line in functional.splitlines() if ' report ' in line]
        self.assertEqual(1, len(report))
        self.assertIn('|| echo', report[0])

    def test_the_gate_registered_check_fails_the_step_on_an_unknown_name(self):
        # A name the runner cannot resolve is only its WARNING; here it is a failure.
        with tempfile.TemporaryDirectory() as tmp:
            tests = Path(tmp) / 'functional'
            tests.mkdir()
            (tests / 'test_runner.py').write_text("ALL_SCRIPTS = ['a_test.py', 'b_test.py --legacy-wallet']\n")
            gate = Path(tmp) / 'gate.txt'
            gate.write_text('9 b_test.py  # b\n5 a_test.py  # a\n')
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(0, functional_gate.main(['--gate', str(gate), 'order', '--tests-dir', str(tests)]))
            self.assertEqual('b_test.py\na_test.py\n', out.getvalue())
            gate.write_text('9 b_test.py  # b\n5 c_test.py  # not registered\n')
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(1, functional_gate.main(['--gate', str(gate), 'order', '--tests-dir', str(tests)]))
            self.assertEqual('', out.getvalue())
            self.assertIn('not registered in test_runner.py: c_test.py', err.getvalue())

    def test_the_real_gate_is_registered_and_starts_with_the_longest_test(self):
        # As the workflow calls it: test_runner.py reads the encoding of a real standard output when imported.
        result = subprocess.run([sys.executable, str(ROOT / 'ci/dash/functional_gate.py'), 'order'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        self.assertEqual(0, result.returncode, result.stderr)
        order = result.stdout.split()
        entries = functional_gate.read_gate()
        self.assertEqual(sorted(name for name, _ in entries), sorted(order))
        seconds = dict(entries)
        self.assertEqual(sorted(seconds.values(), reverse=True), [seconds[name] for name in order])

    def test_gate_order_is_longest_first_and_stable(self):
        entries = [('a.py', 5), ('b.py', 50), ('c.py', 5), ('d.py', 70), ('e.py', 50)]
        self.assertEqual(['d.py', 'b.py', 'e.py', 'a.py', 'c.py'],
                         [name for name, _ in functional_gate.longest_first(entries)])

    def test_gate_shards_cover_every_test_once_longest_first(self):
        entries = functional_gate.read_gate()
        for count in range(1, 5):
            with self.subTest(shards=count):
                shards = [functional_gate.shard(entries, index, count) for index in range(1, count + 1)]
                names = [name for part in shards for name, _ in part]
                self.assertEqual(sorted(name for name, _ in entries), sorted(names))
                for part in shards:
                    self.assertTrue(part)
                    self.assertEqual(part, functional_gate.longest_first(part))
                # Longest first to the least loaded shard: no shard exceeds an even split by more than its
                # largest test.
                total = sum(seconds for _, seconds in entries)
                for part in shards:
                    load = sum(seconds for _, seconds in part)
                    self.assertLessEqual(load, max(seconds for _, seconds in part) + total / count)
        for index, count in ((0, 1), (2, 1), (3, 2)):
            with self.subTest(index=index, count=count):
                with self.assertRaises(functional_gate.GateError):
                    functional_gate.shard(entries, index, count)

    def test_malformed_gate_lines_are_refused(self):
        for text in ('',
                     '# only a comment\n',
                     '5 a.py\n',                     # says nothing about what it guards
                     '0 a.py  # zero\n',
                     '05 a.py  # leading zero\n',
                     'x a.py  # no seconds\n',
                     '5 a.py --descriptors  # a variant, not a test\n',
                     '5 a  # no .py\n',
                     '5 a.py  # a\n6 a.py  # twice\n',
                     ' 5 a.py  # indented\n'):
            with self.subTest(text=text):
                with tempfile.TemporaryDirectory() as tmp:
                    gate = Path(tmp) / 'gate.txt'
                    gate.write_text(text)
                    with self.assertRaises(functional_gate.GateError):
                        functional_gate.read_gate(gate)

    def test_duration_report_adds_up_variants_and_flags_drift(self):
        log = textwrap.dedent('''\
            TEST                                     | STATUS    | DURATION

            a_test.py --descriptors                  | ✓ Passed  | 100 s
            a_test.py --legacy-wallet                | ✓ Passed  | 90 s
            2026-11-07T12:59:32.4236323Z b_test.py   | ✖ Failed  | 7 s
            \x1b[0;32mc_test.py                      | ✓ Passed  | 400 s\x1b[0m
            31/37 - e_test.py passed, Duration: 12 s

            ALL                                      | ✖ Failed  | 597 s (accumulated)
            Runtime: 400 s
            ''')
        self.assertEqual({'a_test.py': 190, 'b_test.py': 7, 'c_test.py': 400}, functional_gate.measured(log))
        text = functional_gate.report([('a_test.py', 180), ('b_test.py', 8), ('d_test.py', 3)], log)
        rows = [line for line in text.splitlines() if line.startswith('| ') and '_test.py' in line]
        self.assertEqual(['| c_test.py |  | 400 | not in the list |',
                          '| a_test.py | 180 | 190 |  |',
                          '| b_test.py | 8 | 7 |  |',
                          '| d_test.py | 3 |  | not run |'], rows)
        drift = functional_gate.report([('a_test.py', 40)], log)
        self.assertIn('| a_test.py | 40 | 190 | refresh the recorded seconds |', drift)
        self.assertIn('no closing table', functional_gate.report([('a_test.py', 40)], 'no table here'))

    def test_fuzz_regression_runs_for_code_and_does_not_hold_the_required_check(self):
        fuzz = job_text(self.workflow, 'fuzz')
        self.assertIn('    needs: changes\n', fuzz)
        self.assertIn("    if: needs.changes.outputs.code == 'true'\n", fuzz)
        # Informational for its first weeks: its failure shows as a failed job (no continue-on-error, which would
        # report it as a success), and the required check neither waits for it nor reads it.
        self.assertNotIn('continue-on-error', fuzz)
        required = re.search(r'^    needs: \[(.*)\]$', self.build, re.MULTILINE).group(1)
        self.assertNotIn('fuzz', required)
        self.assertNotIn("'fuzz'", self.gate)
        for step, command in (('Build the fuzz binary', 'ci/dash/fuzz_regression.sh build\n'),
                              ('Fetch the seed corpus', 'ci/dash/fuzz_regression.sh corpus "$RUNNER_TEMP/fuzz-corpus"\n'),
                              ('Run every fuzz target once', 'ci/dash/fuzz_regression.sh run "$RUNNER_TEMP/fuzz-corpus"\n')):
            with self.subTest(step=step):
                self.assertIn('        run: ' + command, step_text(fuzz, step))

    def test_fuzz_regression_script_is_valid_and_pins_its_corpus(self):
        script_path = ROOT / 'ci/dash/fuzz_regression.sh'
        self.assertTrue(os.access(script_path, os.X_OK))
        result = subprocess.run(['bash', '-n', str(script_path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        script = script_path.read_text()
        self.assertRegex(script, r'(?m)^QA_ASSETS_COMMIT=[0-9a-f]{40}$')
        # set -e does not see a failure inside a process substitution: every listing goes through a file.
        self.assertNotIn('<(', script)
        self.assertIn('"$(git -C "$fetched" rev-parse HEAD)" == "$QA_ASSETS_COMMIT"', script)
        self.assertIn('--with-sanitizers=fuzzer,address,undefined,integer', script)

    def test_fuzz_regression_skips_only_the_tracked_failures(self):
        # A target leaves this list with the fix of its tracked cause; none joins it unnoticed.
        script = (ROOT / 'ci/dash/fuzz_regression.sh').read_text()
        listed = re.search(r'(?ms)^EXCLUDED_TARGETS=\((.*?)^\)$', script).group(1).split()
        self.assertEqual(sorted(listed), sorted([
            'rpc',  # its RPC command lists know none of the Dash and DeFCoN commands
        ]))
        self.assertIn('--exclude "$excluded"', script)

    def test_fuzz_regression_lets_no_saved_input_miss_its_target(self):
        # A qa-assets directory is a target here or a named Dash-only one. The run derives what the corpus must hold
        # (the pinned Dash inputs) and trusts nothing the corpus step left in it.
        expected = set((ROOT / 'ci/dash/fuzz_targets.txt').read_text().splitlines())
        dash_only = (ROOT / 'ci/dash/fuzz_qa_assets_dash_only.txt').read_text().splitlines()
        self.assertTrue(dash_only)
        self.assertEqual(dash_only, sorted(set(dash_only)))
        self.assertNotIn('', dash_only)
        self.assertEqual(set(), set(dash_only) & expected)
        script = (ROOT / 'ci/dash/fuzz_regression.sh').read_text()
        self.assertRegex(script, r'(?m)^QA_ASSETS_DASH_ONLY=ci/dash/fuzz_qa_assets_dash_only\.txt$')
        self.assertRegex(script, r'(?m)^QA_ASSETS_USED_INPUTS=[1-9][0-9]*$')
        self.assertRegex(script, r'(?m)^QA_ASSETS_USED_SHA256=[0-9a-f]{64}$')
        self.assertIn('dash_sha256=$(sha256sum < "$work/all" | cut -d\' \' -f1)', script)
        # No own corpus yet (where it will live is open): nothing reads one.
        self.assertNotIn('OWN_CORPUS', script)
        self.assertNotIn('test/fuzz/corpus', script)
        # Anything but a regular file or a directory (a symbolic link, say) fails rather than being left out, and so
        # does a checked directory that is itself a link: find would not look into it, cd and the runner would.
        self.assertIn("find \"$1\" -mindepth 1 ! -type f ! -type d -print -quit", script)
        self.assertIn('not_a_link() { [[ ! -L "$1" ]] || fail "$1 is a symbolic link"; }', script)
        self.assertEqual(2, script.count('  not_a_link "$1"\n'))
        # Both steps use the path exactly as given and refuse any spelling that is not plain: nothing rewrites it.
        self.assertIn('plain_path() {', script)
        # Only a plain absolute path, as the job gives it: never looked up in CDPATH, never read as an option.
        self.assertIn('[[ "$p" == /?* && ', script)
        self.assertIn('    dest=$2\n    plain_path "$dest"\n', script)
        self.assertIn('    corpus=$2\n    plain_path "$corpus"\n', script)
        self.assertNotIn('realpath', script)

    def test_fuzz_regression_expects_a_reviewed_target_list(self):
        # The binary lists only what it was built with: the run first compares that with this reviewed list.
        expected = (ROOT / 'ci/dash/fuzz_targets.txt').read_text().splitlines()
        self.assertTrue(expected)
        self.assertEqual(expected, sorted(set(expected)))
        self.assertNotIn('', expected)
        script = (ROOT / 'ci/dash/fuzz_regression.sh').read_text()
        self.assertRegex(script, r'(?m)^EXPECTED_TARGETS=ci/dash/fuzz_targets\.txt$')
        self.assertIn('diff -u "$EXPECTED_TARGETS"', script)
        excluded = re.search(r'(?ms)^EXCLUDED_TARGETS=\((.*?)^\)$', script).group(1).split()
        self.assertEqual([], sorted(set(excluded) - set(expected)))

    def test_every_multiline_shell_block_has_valid_bash_syntax(self):
        blocks = re.findall(r'^        run: \|\n((?:          [^\n]*\n|\n)*)',
                            self.workflow, re.MULTILINE)
        self.assertGreaterEqual(len(blocks), 10)
        for number, block in enumerate(blocks):
            with self.subTest(block=number):
                script = re.sub(r'\$\{\{.*?\}\}', 'fixture', textwrap.dedent(block))
                result = subprocess.run(['bash', '-n'], input=script, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual(0, result.returncode, result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
