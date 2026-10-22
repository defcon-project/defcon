#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""No-build guards for parallel CI lanes, cache scope and the required check."""

import copy
import json
import os
from pathlib import Path
import re
import subprocess
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/build.yml'


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
        self.assertEqual(['runtime', 'build-options'],
                         re.findall(r'^            lane: (.+)$', self.checks, re.MULTILINE))

    def test_each_product_gate_has_one_lane_and_is_not_removed(self):
        lanes = {
            'runtime': ('Build source and run tests', 'Unit suites (all but the named exceptions)',
                        'Qt tests and leak check', 'Functional tests (named gate)', 'Upload build artifacts'),
            'build-options': ('Prepare the build-options source', 'Configure option regressions',
                              'No-wallet build and RPC/PoS regressions'),
        }
        for lane, names in lanes.items():
            for name in names:
                with self.subTest(step=name):
                    condition = "        if: needs.changes.outputs.code == 'true' && matrix.lane == '" + lane + "'\n"
                    self.assertIn(condition, step_text(self.checks, name))

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
                         '!validation_chainstatemanager_tests:!wallet_tests', selection)

    def test_original_functional_gate_jobs_and_timeout_are_preserved(self):
        functional = run_script(step_text(self.checks, 'Functional tests (named gate)'))
        gate = re.search(r'GATE=\(\n(.*?)\n\)', functional, re.DOTALL).group(1)
        names = [line.split('#', 1)[0].strip() for line in gate.splitlines()]
        self.assertEqual([
            'feature_llmq_chainlocks.py', 'feature_pos_staking.py', 'feature_pos_stake_combine.py',
            'feature_pos_verifychain.py', 'feature_dsl_sign_wait.py', 'feature_dsl_commitment_v2.py',
            'feature_addressindex_coinstake.py', 'p2p_instantsend.py', 'rpc_verifyislock.py', 'rpc_bls.py',
            'feature_dsl_service.py', 'feature_dsl_perf.py', 'feature_dsl_perf_burst.py',
            'feature_llmq_is_nonrotation.py', 'feature_llmq_is_nonrotated_verify.py',
            'feature_llmq_is_named_quorum_window.py', 'feature_superblock_retirement.py',
            'feature_v23_bundle_heights.py', 'feature_llmq_q60_proto_floor.py', 'feature_evodb_reconcile.py',
            'rpc_verifychainlock.py', 'feature_evodb_reconcile_below_dip3.py', 'feature_llmq_q60_regtest.py',
            'feature_llmq_switchover_config.py', 'p2p_headers_uncompressed.py',
            'feature_dsl_commit_decision_log.py',
        ], names)
        self.assertIn('--jobs=4 --timeout-factor="${TEST_RUNNER_TIMEOUT_FACTOR}"', functional)
        self.assertNotIn('--failfast', functional)

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
