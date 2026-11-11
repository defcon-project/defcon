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
import threading
import types
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/build.yml'
sys.path.insert(0, str(ROOT / 'ci/dash'))
import functional_gate  # noqa: E402
import unit_suites  # noqa: E402


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


UPSTREAM = 'defcon-project/defcon'
NOT_A_FORK_PUSH = "github.event_name != 'push' || github.repository == 'defcon-project/defcon'"
MIRROR_STEP = 'Wait for the upstream required check on this commit'

# The mirror job's tools, faked. A clock in $FAKE_DIR/clock: `date +%s` prints it and `sleep` moves it on. `timeout`
# hands its limit to the command. `gh api --paginate <url> --jq <filter>` answers from canned responses kept per
# endpoint -- the runs on the commit, the jobs of one attempt -- the last one repeating, printed the way the job's
# filters print them (a runs answer may name another workflow file, which the filter drops). Each gh call takes one
# second of the clock (or the answer's "takes") and is recorded with the time it started; a "hang" answer takes the
# whole limit and exits 124, as timeout would end it; an "error" answer writes to stderr and exits 1. `date` can also
# move the clock on by FAKE_TICK seconds a call, standing for the time the script spends between its calls.
FAKE_GH = r'''#!/usr/bin/env python3
import json
import os
import sys

home = os.environ['FAKE_DIR']
clock_file = os.path.join(home, 'clock')
now = int(open(clock_file, encoding='utf8').read())
kind = 'jobs' if '/attempts/' in sys.argv[3] else 'runs'
with open(os.path.join(home, 'calls'), 'a', encoding='utf8') as calls:
    calls.write(json.dumps({'at': now, 'kind': kind, 'args': sys.argv[1:]}) + '\n')
counter = os.path.join(home, 'count-' + kind)
number = int(open(counter, encoding='utf8').read()) if os.path.exists(counter) else 0
with open(counter, 'w', encoding='utf8') as out:
    out.write(str(number + 1))
answers = json.load(open(os.path.join(home, 'responses.json'), encoding='utf8'))[kind]
answer = answers[min(number, len(answers) - 1)]
cap = int(os.environ['FAKE_CAP'])
cut = 'hang' in answer or answer.get('takes', 1) > cap
with open(clock_file, 'w', encoding='utf8') as out:
    out.write(str(now + (cap if cut else answer.get('takes', 1))))
if cut:
    sys.exit(124)
if 'error' in answer:
    sys.stderr.write(answer['error'] + '\n')
    sys.exit(1)
if kind == 'runs':
    for run in answer['runs']:
        if run['path'] == '.github/workflows/build.yml':
            print('\t'.join([str(run['id']), run['status'], str(run['attempt']),
                             'https://github.com/defcon-project/defcon/actions/runs/%d' % run['id']]))
else:
    for job in answer['jobs']:
        print('\t'.join([str(job['id']), job['status'], job['conclusion'] or 'none',
                         'https://github.com/defcon-project/defcon/actions/runs/1/job/%d' % job['id'], job['name']]))
'''
FAKE_DATE = ('#!/bin/sh\nnow=$(cat "$FAKE_DIR/clock")\necho "$now"\n'
             'echo $((now + ${FAKE_TICK:-0})) > "$FAKE_DIR/clock"\n')
FAKE_SLEEP = '#!/bin/sh\necho "$1" >> "$FAKE_DIR/sleeps"\nnow=$(( $(cat "$FAKE_DIR/clock") + $1 ))\necho "$now" > "$FAKE_DIR/clock"\n'
FAKE_TIMEOUT = '#!/bin/sh\ncap=$1\nshift\necho "$cap" >> "$FAKE_DIR/timeouts"\nFAKE_CAP=$cap exec "$@"\n'
RUNS_FILTER = ('.workflow_runs[] | select(.path == ".github/workflows/build.yml") '
               '| [.id, .status, .run_attempt, .html_url] | @tsv')
JOBS_FILTER = '.jobs[] | [.id, .status, (.conclusion // "none"), .html_url, .name] | @tsv'
REQUIRED = 'Build (linux64, x86_64-pc-linux-gnu, linux64)'
COMMIT = 'c' * 40
START = 1000000


def workflow_jobs(workflow):
    """Each job's condition (None without one) and needs, in the order the jobs appear."""
    section = workflow[workflow.index('\njobs:\n'):]
    jobs = {}
    for name in re.findall(r'^  ([\w-]+):\n', section, re.MULTILINE):
        text = job_text(workflow, name)
        condition = re.search(r'^    if: (.+)$', text, re.MULTILINE)
        needs = re.search(r'^    needs: (.+)$', text, re.MULTILINE)
        listed = needs.group(1).strip() if needs else ''
        if listed.startswith('['):
            listed = listed[1:-1]
        jobs[name] = (condition.group(1).strip() if condition else None,
                      [item.strip() for item in listed.split(',') if item.strip()])
    return jobs


def evaluate(condition, event, repository):
    """(value, has a status function) of a job condition, for the expressions this workflow uses. Anything this does
    not know -- another context, another function, a bare `!` -- fails to evaluate, so a new kind of condition needs
    a matching change here rather than passing unnoticed."""
    expression = condition
    if expression.startswith('${{') and expression.endswith('}}'):
        expression = expression[3:-2].strip()
    status = 'always()' in expression
    python = (expression.replace('always()', 'True').replace('&&', ' and ').replace('||', ' or ')
              .replace('github.event_name', repr(event)).replace('github.repository', repr(repository))
              .replace('needs.changes.outputs.code', repr('true')))
    return bool(eval(python, {'__builtins__': {}}, {})), status  # a fixed expression from the workflow


def running_jobs(workflow, event, repository):
    """The jobs a run starts for this event in this repository, when every job that starts succeeds: a job runs when
    its condition holds and, unless the condition has a status function, every job it needs ran."""
    jobs = workflow_jobs(workflow)
    ran = {}
    while len(ran) < len(jobs):
        progress = False
        for name, (condition, needs) in jobs.items():
            if name in ran or any(need not in ran for need in needs):
                continue
            value, status = evaluate(condition, event, repository) if condition else (True, False)
            ran[name] = value and (status or all(ran[need] for need in needs))
            progress = True
        if not progress:
            raise AssertionError('jobs with unknown or circular needs: %s' % sorted(set(jobs) - set(ran)))
    return {name for name, started in ran.items() if started}


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
        # always(), except on a fork's push, where nothing is built.
        self.assertIn('    if: ${{ always() && (' + NOT_A_FORK_PUSH + ') }}\n', self.build)
        self.assertIn('    needs: [changes, build-image, build-depends, checks]\n', self.build)
        self.assertIn('          NEEDS_JSON: ${{ toJSON(needs) }}\n', self.build)

    def test_a_fork_push_tests_nothing_and_only_refreshes_the_image(self):
        everything = set(workflow_jobs(self.workflow)) - {'upstream-result'}
        self.assertEqual({'changes', 'build-image', 'build-depends', 'checks', 'fuzz', 'build'}, everything)
        for event in ('push', 'pull_request'):
            with self.subTest(event=event, repository=UPSTREAM):
                self.assertEqual(everything, running_jobs(self.workflow, event, UPSTREAM))
        # A pull request opened inside a fork still starts every job, in the fork's own CI image.
        self.assertEqual(everything, running_jobs(self.workflow, 'pull_request', 'minuszka/defcon'))
        # A fork's push detects changes and refreshes that image -- nothing else would -- and builds and tests
        # nothing: the mirror shows this repository's result instead.
        for fork in ('minuszka/defcon', 'someone/defcon'):
            with self.subTest(event='push', repository=fork):
                self.assertEqual({'changes', 'build-image', 'upstream-result'},
                                 running_jobs(self.workflow, 'push', fork))
        self.assertIn("        if: github.event_name == 'push' && needs.changes.outputs.code == 'true'\n",
                      step_text(job_text(self.workflow, 'build-image'), 'Build and push native Linux CI image'))
        self.assertIn("    if: " + NOT_A_FORK_PUSH + "\n", job_text(self.workflow, 'build-depends'))
        self.assertIn('    needs: [changes, build-image, build-depends]\n', self.checks)

    def test_the_condition_evaluator_refuses_what_it_does_not_know(self):
        self.assertEqual((True, True), evaluate('${{ always() && (' + NOT_A_FORK_PUSH + ') }}', 'push', UPSTREAM))
        self.assertEqual((False, False), evaluate(NOT_A_FORK_PUSH, 'push', 'minuszka/defcon'))
        for unknown in ('${{ !cancelled() }}', 'github.ref == "x"', 'failure()', "needs.checks.result == 'success'"):
            with self.subTest(condition=unknown):
                with self.assertRaises(Exception):
                    evaluate(unknown, 'push', UPSTREAM)

    def test_the_mirror_reads_this_repositorys_required_check(self):
        mirror = job_text(self.workflow, 'upstream-result')
        self.assertIn('    name: Upstream CI result (mirrored)\n', mirror)
        self.assertNotIn('    needs:', mirror)
        step = step_text(mirror, MIRROR_STEP)
        required = re.search(r'^    name: (.+)$', self.build, re.MULTILINE).group(1)
        self.assertIn('          CHECK_NAME: ' + required + '\n', step)
        self.assertIn('          UPSTREAM: ' + UPSTREAM + '\n', step)
        self.assertIn('          COMMIT: ${{ github.sha }}\n', step)
        self.assertIn('          GH_TOKEN: ${{ github.token }}\n', step)
        self.assertIn('        shell: bash\n', step)
        # Its own deadline comes first, so running out of time is a failure that says why, not a cancelled job.
        timeout = int(re.search(r'^    timeout-minutes: (\d+)$', mirror, re.MULTILINE).group(1))
        wait = int(re.search(r'^          WAIT_MINUTES: (\d+)$', step, re.MULTILINE).group(1))
        self.assertLess(wait, timeout)

    def run_mirror(self, runs, jobs=({'jobs': []},), **overrides):
        """Runs the mirror step against the fake tools, with canned answers for the runs and the jobs endpoints."""
        step = step_text(job_text(self.workflow, 'upstream-result'), MIRROR_STEP)
        script = run_script(step)
        block = re.search(r'^        env:\n((?:          [^\n]*\n)+)', step, re.MULTILINE).group(1)
        settings = dict(re.findall(r'^          ([A-Z_]+): (.+)$', block, re.MULTILINE))
        self.assertEqual({'GH_TOKEN', 'UPSTREAM', 'CHECK_NAME', 'COMMIT', 'POLL_SECONDS', 'APPEAR_MINUTES',
                          'WAIT_MINUTES', 'MAX_API_ERRORS'}, set(settings))
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            tools = home / 'bin'
            tools.mkdir()
            for name, text in (('gh', FAKE_GH), ('date', FAKE_DATE), ('sleep', FAKE_SLEEP), ('timeout', FAKE_TIMEOUT)):
                (tools / name).write_text(text)
                (tools / name).chmod(0o755)
            (home / 'clock').write_text(str(START))
            (home / 'responses.json').write_text(json.dumps({'runs': list(runs), 'jobs': list(jobs)}))
            summary = home / 'summary.md'
            summary.write_text('')
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
            env.update({key: value for key, value in settings.items() if '${{' not in value})
            env.update(GH_TOKEN='fake', COMMIT=COMMIT, GITHUB_STEP_SUMMARY=str(summary), RUNNER_TEMP=tmp,
                       FAKE_DIR=tmp, PATH=str(tools) + os.pathsep + os.environ['PATH'])
            env.update({key: str(value) for key, value in overrides.items()})
            result = subprocess.run(['bash', '-c', script], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=60)

            def lines(name):
                path = home / name
                return path.read_text().splitlines() if path.exists() else []
            return types.SimpleNamespace(
                status=result.returncode, output=result.stdout, summary=summary.read_text(),
                calls=[json.loads(line) for line in lines('calls')], sleeps=[int(s) for s in lines('sleeps')],
                timeouts=[int(t) for t in lines('timeouts')], end=int((home / 'clock').read_text()))

    @staticmethod
    def upstream_run(number, status, attempt=1, path='.github/workflows/build.yml'):
        return {'id': number, 'status': status, 'attempt': attempt, 'path': path}

    @staticmethod
    def job(number, status, conclusion=None, name=REQUIRED):
        return {'id': number, 'status': status, 'conclusion': conclusion, 'name': name}

    def answered(self, *conclusions):
        """Jobs answers of finished attempts: change detection, then the required check, concluded as given."""
        return [{'jobs': [self.job(5, 'completed', 'success', 'Detect changes'), self.job(9, 'completed', conclusion)]}
                for conclusion in conclusions]

    @staticmethod
    def jobs_urls(result):
        return [call['args'][2] for call in result.calls if call['kind'] == 'jobs']

    def test_the_mirror_reads_the_runs_then_the_jobs_of_that_exact_attempt(self):
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed', attempt=3)]}], self.answered('success'))
        self.assertEqual(0, result.status, result.output)
        runs = ['api', '--paginate', 'repos/defcon-project/defcon/actions/runs?head_sha=' + COMMIT + '&per_page=100',
                '--jq', RUNS_FILTER]
        self.assertEqual([runs, ['api', '--paginate', 'repos/defcon-project/defcon/actions/runs/42/attempts/3/jobs'
                                 '?per_page=100', '--jq', JOBS_FILTER], runs],
                         [call['args'] for call in result.calls])
        self.assertEqual([60, 60, 60], result.timeouts)

    def test_the_mirror_passes_and_fails_with_the_upstream_check(self):
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed', attempt=2)]}], self.answered('success'))
        self.assertEqual((0, []), (result.status, result.sleeps))
        self.assertIn('[' + REQUIRED + '](https://github.com/defcon-project/defcon/actions/runs/1/job/9) on `'
                      + COMMIT + '`, attempt 2: **success**', result.summary)
        for conclusion in ('failure', 'cancelled', 'timed_out', 'action_required', 'skipped', 'neutral'):
            with self.subTest(conclusion=conclusion):
                result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed')]}], self.answered(conclusion))
                self.assertEqual(1, result.status)
                self.assertIn('::error::The upstream required check concluded ' + conclusion, result.output)
                self.assertIn('re-run this job', result.output)
                self.assertIn('**' + conclusion + '**', result.summary)

    def test_the_mirror_waits_out_a_re_run_and_reads_only_its_attempt(self):
        # While a re-run goes, its run is in progress again, whatever the earlier attempt's required check said:
        # nothing is read from any attempt until the run has finished, and then only the newest attempt counts. A
        # partial re-run lists the jobs it reused next to the ones it ran again.
        for new in ('success', 'failure'):
            with self.subTest(re_run=new):
                result = self.run_mirror(
                    [{'runs': [self.upstream_run(42, 'in_progress', attempt=2)]}, {'runs': [self.upstream_run(42, 'queued', attempt=2)]},
                     {'runs': [self.upstream_run(42, 'completed', attempt=2)]}],
                    [{'jobs': [self.job(5, 'completed', 'success', 'Detect changes'),
                               self.job(7, 'completed', 'success', 'Checks (linux64, x86_64-pc-linux-gnu, linux64, runtime)'),
                               self.job(9, 'completed', new)]}])
                self.assertEqual(0 if new == 'success' else 1, result.status, result.output)
                self.assertEqual(['repos/defcon-project/defcon/actions/runs/42/attempts/2/jobs?per_page=100'],
                                 self.jobs_urls(result))
                self.assertEqual(2, len(result.sleeps))

    def test_the_mirror_does_not_take_an_answer_from_an_attempt_a_re_run_replaced(self):
        # A re-run begins after the runs were read: attempt 1's jobs stay finished, with either conclusion, but the
        # runs read again after them show attempt 2, so attempt 1's answer is not taken; attempt 2 decides.
        for old, new in (('success', 'failure'), ('failure', 'success')):
            with self.subTest(old=old, new=new):
                result = self.run_mirror(
                    [{'runs': [self.upstream_run(42, 'completed', attempt=1)]},
                     {'runs': [self.upstream_run(42, 'in_progress', attempt=2)]},
                     {'runs': [self.upstream_run(42, 'completed', attempt=2)]}],
                    self.answered(old, new))
                self.assertEqual(0 if new == 'success' else 1, result.status, result.output)
                self.assertIn('changed while they were read', result.output)
                self.assertEqual(['repos/defcon-project/defcon/actions/runs/42/attempts/1/jobs?per_page=100',
                                  'repos/defcon-project/defcon/actions/runs/42/attempts/2/jobs?per_page=100'],
                                 self.jobs_urls(result))
                self.assertIn('**' + new + '**', result.summary)
                self.assertNotIn('**' + old + '**', result.summary)
        # Re-run and finished between the reads: the attempt changed, so the same.
        result = self.run_mirror(
            [{'runs': [self.upstream_run(42, 'completed', attempt=1)]},
             {'runs': [self.upstream_run(42, 'completed', attempt=2)]}],
            self.answered('success', 'failure'))
        self.assertEqual(1, result.status, result.output)
        # A new run on the commit between the reads (a tag pushed) also sends it back to reading.
        result = self.run_mirror(
            [{'runs': [self.upstream_run(40, 'completed')]},
             {'runs': [self.upstream_run(40, 'completed'), self.upstream_run(44, 'queued')]},
             {'runs': [self.upstream_run(40, 'completed'), self.upstream_run(44, 'completed')]}],
            self.answered('success', 'failure'))
        self.assertEqual(1, result.status, result.output)
        self.assertEqual('repos/defcon-project/defcon/actions/runs/44/attempts/1/jobs?per_page=100',
                         self.jobs_urls(result)[-1])

    def test_the_mirror_waits_when_the_attempt_s_check_is_not_finished(self):
        result = self.run_mirror(
            [{'runs': [self.upstream_run(42, 'completed', attempt=1)]}],
            [{'jobs': [self.job(9, 'in_progress')]}, {'jobs': [self.job(9, 'completed', 'success')]}])
        self.assertEqual(0, result.status, result.output)
        self.assertIn("Attempt 1's required check is in_progress: waiting", result.output)

    def test_the_mirror_waits_for_every_run_on_the_commit_and_the_newest_decides(self):
        # A release branch push and a tag push on one commit make two runs.
        result = self.run_mirror(
            [{'runs': [self.upstream_run(44, 'in_progress'), self.upstream_run(40, 'completed')]},
             {'runs': [self.upstream_run(44, 'completed'), self.upstream_run(40, 'completed')]}],
            self.answered('failure'))
        self.assertEqual(1, result.status, result.output)
        self.assertEqual(['repos/defcon-project/defcon/actions/runs/44/attempts/1/jobs?per_page=100'],
                         self.jobs_urls(result))
        self.assertEqual(1, len(result.sleeps))
        # The newest by id, in whatever order the runs are listed.
        result = self.run_mirror(
            [{'runs': [self.upstream_run(40, 'completed', attempt=2), self.upstream_run(44, 'completed')]}],
            self.answered('success'))
        self.assertEqual(0, result.status, result.output)
        self.assertEqual(['repos/defcon-project/defcon/actions/runs/44/attempts/1/jobs?per_page=100'],
                         self.jobs_urls(result))

    def test_the_mirror_ignores_other_workflows_and_similar_names(self):
        result = self.run_mirror(
            [{'runs': [self.upstream_run(45, 'in_progress', path='.github/workflows/macos-smoke.yml'),
                       self.upstream_run(41, 'completed')]}],
            [{'jobs': [self.job(5, 'completed', 'success', 'Build Image'),
                       self.job(6, 'completed', 'success', REQUIRED + ' '),
                       self.job(4, 'completed', 'failure')]}])
        self.assertEqual((1, []), (result.status, result.sleeps), result.output)
        self.assertEqual(['repos/defcon-project/defcon/actions/runs/41/attempts/1/jobs?per_page=100'],
                         self.jobs_urls(result))

    def test_the_mirror_fails_when_a_finished_run_has_no_required_check(self):
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed')]}],
                                 [{'jobs': [self.job(5, 'completed', 'success', 'Detect changes')]}])
        self.assertEqual(1, result.status)
        self.assertIn('finished without the required check', result.output)

    def test_the_mirror_fails_for_a_commit_upstream_never_ran(self):
        result = self.run_mirror([{'runs': []}], APPEAR_MINUTES=0)
        self.assertEqual(1, result.status)
        self.assertIn('::error::defcon-project/defcon has no CI run on ' + COMMIT + ' after 0 minutes: it never '
                      'tested this commit.', result.output)
        # Within the appearance window it keeps waiting: the push may not have reached upstream yet.
        result = self.run_mirror([{'runs': []}, {'runs': [self.upstream_run(42, 'completed')]}], self.answered('success'))
        self.assertEqual((0, 1), (result.status, len(result.sleeps)), result.output)

    def test_the_mirror_rides_out_a_few_failed_reads_but_not_many(self):
        outage = {'error': 'gh: HTTP 502: Bad Gateway'}
        result = self.run_mirror([outage, outage, {'runs': [self.upstream_run(42, 'completed')]}], self.answered('success'),
                                 MAX_API_ERRORS=3)
        self.assertEqual(0, result.status, result.output)
        self.assertIn('::warning::Reading the upstream CI state failed (2 in a row): gh: HTTP 502', result.output)
        result = self.run_mirror([outage], MAX_API_ERRORS=3)
        self.assertEqual((1, 3), (result.status, len(result.calls)), result.output)
        self.assertIn('::error::Reading the upstream CI state failed 3 times in a row.', result.output)
        # An answered runs read does not reset the count while every jobs read fails.
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed')]}], [outage], MAX_API_ERRORS=3)
        self.assertEqual((1, 3), (result.status, len(self.jobs_urls(result))), result.output)
        # A round with every read answered starts the count again.
        result = self.run_mirror([outage, outage, {'runs': [self.upstream_run(42, 'in_progress')]}, outage, outage,
                                  {'runs': [self.upstream_run(42, 'completed')]}], self.answered('success'), MAX_API_ERRORS=3)
        self.assertEqual(0, result.status, result.output)

    def test_the_mirror_keeps_its_own_deadline(self):
        # Polling a run that never finishes: no read starts at or after the deadline, the last pause is cut short to
        # end on it, and the job fails there.
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'in_progress')]}], WAIT_MINUTES=5)
        self.assertEqual(1, result.status)
        self.assertIn('::error::The upstream required check did not finish within 5 minutes.', result.output)
        self.assertTrue(all(call['at'] < START + 300 for call in result.calls), result.calls)
        self.assertEqual(START + 300, result.end)
        self.assertTrue(all(0 < pause <= 60 for pause in result.sleeps), result.sleeps)
        # A result that would only arrive after the deadline is never read.
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'in_progress')]}] * 2 + [{'runs': [self.upstream_run(42, 'completed')]}],
                                 self.answered('success'), WAIT_MINUTES=2)
        self.assertEqual(1, result.status, result.output)
        self.assertEqual([], self.jobs_urls(result))

    def test_the_mirror_takes_no_verdict_after_its_deadline(self):
        # The second runs read answers exactly at the deadline, inside its own limit: the verdict, a success here,
        # is not taken.
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed')], 'takes': 30},
                                  {'runs': [self.upstream_run(42, 'completed')], 'takes': 1}],
                                 [{'jobs': [self.job(9, 'completed', 'success')], 'takes': 29}], WAIT_MINUTES=1)
        self.assertEqual([START, START + 30, START + 59], [call['at'] for call in result.calls])
        self.assertEqual(1, result.status, result.output)
        self.assertIn('::error::The upstream required check did not finish within 1 minutes.', result.output)
        self.assertEqual('', result.summary)
        # Time spent between the calls counts as well.
        for tick in (1, 2, 3):
            with self.subTest(tick=tick):
                result = self.run_mirror([{'runs': [self.upstream_run(42, 'completed')], 'takes': 54}],
                                         self.answered('success'), WAIT_MINUTES=1, FAKE_TICK=tick)
                if result.status == 0:
                    self.assertLess(result.end - tick, START + 60, result.output)
                else:
                    self.assertIn('did not finish within 1 minutes', result.output)
                    self.assertEqual('', result.summary)

    def test_the_mirror_cuts_off_a_read_that_does_not_answer(self):
        hang = {'hang': True}
        result = self.run_mirror([hang], WAIT_MINUTES=3)
        self.assertEqual(1, result.status)
        self.assertEqual([60, 60], result.timeouts)
        self.assertEqual(START + 180, result.end)
        self.assertIn('no answer within 60 s', result.output)
        self.assertIn('::error::The upstream required check did not finish within 3 minutes.', result.output)
        # Near the deadline a read gets only the time left.
        result = self.run_mirror([{'runs': [self.upstream_run(42, 'in_progress')]}, hang], WAIT_MINUTES=1, POLL_SECONDS=45)
        self.assertEqual(1, result.status)
        self.assertEqual([60, 14], result.timeouts)
        self.assertEqual(START + 60, result.end)

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
        self.assertEqual(['runtime', 'functional', 'functional', 'functional', 'build-options'],
                         re.findall(r'^            lane: (.+)$', self.checks, re.MULTILINE))

    def test_the_functional_runners_take_every_shard_exactly_once(self):
        # A shard no runner takes is a part of the gate that stopped running, under a green check.
        count = int(re.search(r'^      FUNCTIONAL_SHARDS: ([0-9]+)$', self.checks, re.MULTILINE).group(1))
        self.assertGreaterEqual(count, 1)
        include = re.search(r'^        include:\n((?:          [^\n]*\n)+)', self.checks, re.MULTILINE).group(1)
        runners = [dict(re.findall(r'^ +(?:- )?(\w+): (.+)$', entry, re.MULTILINE))
                   for entry in re.split(r'(?m)^(?=          - )', include) if entry]
        self.assertEqual(count + 2, len(runners))
        self.assertEqual([str(index) for index in range(1, count + 1)],
                         [runner.get('shard') for runner in runners if runner['lane'] == 'functional'])
        self.assertEqual([], [runner for runner in runners if runner['lane'] != 'functional' and 'shard' in runner])
        # The step hands its runner's number and the count to the gate, for the order and for the report.
        step = step_text(self.checks, 'Functional tests (named gate)')
        self.assertIn('        env:\n          SHARD: ${{ matrix.shard }}\n', step)
        functional = run_script(step)
        self.assertIn('shard="$SHARD/$FUNCTIONAL_SHARDS"\n', functional)
        self.assertEqual(2, functional.count('--shard "$shard"'))
        # Two failed shards must not fight over one artifact name.
        self.assertIn('-shard${{ matrix.shard }}\n', step_text(self.checks, 'Upload failed functional test directories'))
        # The real list, dealt as the workflow deals it: every test in exactly one shard, none empty.
        entries = functional_gate.read_gate()
        dealt = [functional_gate.shard(entries, index, count) for index in range(1, count + 1)]
        self.assertTrue(all(dealt))
        self.assertEqual(sorted(entries), sorted(entry for part in dealt for entry in part))

    def test_a_missing_or_foreign_shard_number_runs_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            tests = Path(tmp) / 'functional'
            tests.mkdir()
            (tests / 'test_runner.py').write_text("ALL_SCRIPTS = ['a_test.py', 'b_test.py', 'c_test.py']\n")
            gate = Path(tmp) / 'gate.txt'
            gate.write_text('9 a_test.py  # a\n5 b_test.py  # b\n2 c_test.py  # c\n')
            log = Path(tmp) / 'log'
            log.write_text('b_test.py | ✓ Passed | 6 s\n')

            def call(command, shard):
                out, err = io.StringIO(), io.StringIO()
                arguments = ['--gate', str(gate), command, '--shard', shard]
                arguments += ['--tests-dir', str(tests)] if command == 'order' else [str(log)]
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    status = functional_gate.main(arguments)
                return status, out.getvalue(), err.getvalue()

            self.assertEqual((0, 'b_test.py\n', ''), call('order', '2/3'))
            status, text, _ = call('report', '2/3')
            self.assertEqual(0, status)
            # Only this shard's tests: the other shards' are not reported as "not run".
            self.assertEqual(['| b_test.py | 5 | 6 |  |'], [line for line in text.splitlines() if '_test.py' in line])
            # Four shards of three tests: the fourth holds none.
            for shard in ('/3', '0/3', '4/3', '4/4', '1/', '1/0', 'x/3', '1/3/3', '+1/3', ''):
                for command in ('order', 'report'):
                    with self.subTest(shard=shard, command=command):
                        status, text, error = call(command, shard)
                        self.assertEqual((1, ''), (status, text))
                        self.assertNotEqual('', error)

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
        # The exceptions are the ones the single process was given as '!name' filters, and nothing else selects.
        unit = run_script(step_text(self.checks, 'Unit suites (all but the named exceptions)'))
        self.assertEqual(['bip324_tests:bip39_tests:blockencodings_tests:denialofservice_tests:'
                          'descriptor_tests:dynamic_activation_thresholds_tests:evo_utils_tests:'
                          'governance_validators_tests:key_io_tests:miner_tests/CreateNewBlock_validity:'
                          'transaction_tests:util_tests:validation_chainstate_tests'],
                         re.findall(r"--exclude='([^']+)'", unit))
        self.assertNotIn('--run_test', unit)
        self.assertEqual(1, unit.count('./src/test/test_defcon'))
        self.assertIn('python3 "$GITHUB_WORKSPACE/ci/dash/unit_suites.py" run --binary ./src/test/test_defcon '
                      '--jobs "$(nproc)" \\\n', unit)
        self.assertEqual(['dsl_service_pose_tests'], re.findall(r'--split=(\S+)', unit))
        # Every process gets what the single one got.
        self.assertTrue(unit.rstrip().endswith('-- --catch_system_errors=no -l test_suite'))

    LISTING = textwrap.dedent('''\
        alpha_tests*
            one*
            two*
        beta_tests*
            only*
        gamma_tests*
            first*
            second*
            third*
        ''')

    def test_unit_plan_runs_every_listed_case_once_but_the_exceptions(self):
        tree = unit_suites.read_listing(self.LISTING)
        self.assertEqual({'alpha_tests': {'one': {}, 'two': {}}, 'beta_tests': {'only': {}},
                          'gamma_tests': {'first': {}, 'second': {}, 'third': {}}}, tree)

        def planned(*args, **kwargs):
            return [(unit.selection, unit.cases) for unit in unit_suites.plan(tree, *args, **kwargs)]

        self.assertEqual([('alpha_tests', 2), ('beta_tests', 1), ('gamma_tests', 3)], planned())
        self.assertEqual([('alpha_tests', 2), ('gamma_tests:!gamma_tests/second', 2)],
                         planned(['beta_tests', 'gamma_tests/second']))
        # A split suite's cases come first, one process each, without the excepted one.
        self.assertEqual([('gamma_tests/first', 1), ('gamma_tests/third', 1), ('alpha_tests', 2), ('beta_tests', 1)],
                         planned(['gamma_tests/second'], split=['gamma_tests']))
        for excluded, split in ((['delta_tests'], []),                    # no such suite
                                (['alpha_tests/three'], []),              # no such case
                                (['alpha_tests/'], []),
                                (['alpha_tests/one/x'], []),
                                (['/one'], []),
                                (['alpha_tests', 'alpha_tests/one'], []),
                                (['alpha_tests/one', 'alpha_tests/one'], []),
                                (['beta_tests/only'], []),                # every case of it: except the suite
                                (['alpha_tests', 'beta_tests', 'gamma_tests'], []),
                                ([], ['delta_tests']),
                                ([], ['alpha_tests', 'alpha_tests']),
                                (['alpha_tests'], ['alpha_tests'])):
            with self.subTest(excluded=excluded, split=split):
                with self.assertRaises(unit_suites.GateError):
                    unit_suites.plan(tree, excluded, split)

    def test_unit_listing_that_cannot_be_read_is_refused(self):
        for text in ('',
                     '\n',
                     'alpha_tests\n    one*\n',             # off by default
                     'alpha_tests*\n    one\n',
                     'alpha_tests*\n        one*\n',        # indented past its parent
                     '    one*\n',
                     'alpha_tests*\n  one*\n',              # not four spaces a level
                     'alpha_tests*\n    one<int>*\n',       # a name a filter would need escaping for
                     'alpha_tests*: a description\n',
                     'alpha_tests*\n    one*\n    one*\n',
                     'alpha_tests*\nalpha_tests*\n'):
            with self.subTest(text=text):
                with self.assertRaises(unit_suites.GateError):
                    unit_suites.read_listing(text)
        # Deeper suites are read, and counted by their cases.
        tree = unit_suites.read_listing('a*\n    b*\n        c*\n        d*\n    e*\nf*\n')
        self.assertEqual({'a': {'b': {'c': {}, 'd': {}}, 'e': {}}, 'f': {}}, tree)
        self.assertEqual([('a', 3), ('f', 1)], [(unit.selection, unit.cases) for unit in unit_suites.plan(tree)])
        self.assertEqual([('a/b', 2), ('a/e', 1), ('f', 1)],
                         [(unit.selection, unit.cases) for unit in unit_suites.plan(tree, split=['a'])])

    def test_unit_processes_overlap_up_to_the_job_count_and_start_in_order(self):
        units = [unit_suites.Unit(name, 1) for name in 'abcdef']
        gate, lock, state = threading.Barrier(3, timeout=30), threading.Lock(), {'now': 0, 'most': 0, 'order': []}

        def runner(unit):
            with lock:
                state['now'] += 1
                state['most'] = max(state['most'], state['now'])
                state['order'].append(unit.selection)
            try:
                gate.wait()  # passes only once three run at the same time
            finally:
                with lock:
                    state['now'] -= 1
            return unit, 0.0, None, ''

        out = io.StringIO()
        results = unit_suites.run(units, 3, runner, out)
        self.assertEqual(3, state['most'])
        self.assertEqual(sorted('abcdef'), sorted(unit.selection for unit, _, _ in results))
        self.assertEqual(['a', 'b', 'c'], sorted(state['order'][:3]))
        self.assertEqual(6, out.getvalue().count('::group::'))
        state.update(now=0, most=0, order=[])
        gate = threading.Barrier(1)
        unit_suites.run(units, 1, runner, io.StringIO())
        self.assertEqual((1, list('abcdef')), (state['most'], state['order']))

    # A test binary, faked: the listing above on standard error, and for --run_test the cases the filter selects,
    # as Boost.Test reports them. FAKE_FAIL names a case that fails; FAKE_SHORT one that is silently not run while
    # the exit stays 0; every start is recorded.
    FAKE_BINARY = r'''#!/usr/bin/env python3
import os
import sys

LISTING = %r
TREE, suite = {}, None
for line in LISTING.splitlines():
    if line.startswith('    '):
        TREE[suite].append(line.strip().rstrip('*'))
    else:
        suite = line.rstrip('*')
        TREE[suite] = []
if sys.argv[1:] == ['--list_content']:
    sys.stderr.write(os.environ.get('FAKE_LISTING', LISTING))
    sys.exit(int(os.environ.get('FAKE_LIST_EXIT', '0')))
selection = [arg for arg in sys.argv[1:] if arg.startswith('--run_test=')][0][len('--run_test='):]
with open(os.path.join(os.environ['FAKE_DIR'], 'starts'), 'a', encoding='utf8') as starts:
    starts.write(' '.join(sys.argv[1:]) + '\n')
chosen = []
for part in selection.split(':'):
    name, _, case = part.lstrip('!').partition('/')
    cases = [name + '/' + one for one in TREE.get(name, []) if not case or one == case]
    chosen = [one for one in chosen if one not in cases] if part.startswith('!') else chosen + cases
if not chosen:
    print('Test setup error: no test cases matching filter or all test cases were disabled')
    sys.exit(200)
ran = [one for one in chosen if one != os.environ.get('FAKE_SHORT')]
print('Running %%d test case%%s...' %% (len(ran), '' if len(ran) == 1 else 's'))
for one in ran:
    print('\x1b[1;34;49mEntering test case "%%s"\x1b[0;39;49m' %% one)
if os.environ.get('FAKE_FAIL') in ran:
    print('*** 1 failure is detected in the test module')
    sys.exit(201)
print('\x1b[1;32;49m*** No errors detected\x1b[0;39;49m')
''' % LISTING

    def run_unit_gate(self, *arguments, **env):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'test_fake'
            binary.write_text(self.FAKE_BINARY)
            binary.chmod(0o755)
            summary = Path(tmp) / 'summary'
            result = subprocess.run(
                [sys.executable, str(ROOT / 'ci/dash/unit_suites.py'), 'run', '--binary', str(binary), *arguments],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', FAKE_DIR=tmp, GITHUB_STEP_SUMMARY=str(summary), **env))
            starts = (Path(tmp) / 'starts').read_text().splitlines() if (Path(tmp) / 'starts').exists() else []
            return result, starts, summary.read_text() if summary.exists() else ''

    def test_unit_gate_runs_the_listed_cases_and_passes_only_when_each_ran(self):
        result, starts, summary = self.run_unit_gate(
            '--jobs', '2', '--exclude=beta_tests:gamma_tests/second', '--split=gamma_tests', '--', '-l', 'test_suite')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(['--run_test=alpha_tests -l test_suite', '--run_test=gamma_tests/first -l test_suite',
                          '--run_test=gamma_tests/third -l test_suite'], sorted(starts))
        self.assertEqual(sorted(['alpha_tests/one', 'alpha_tests/two', 'gamma_tests/first', 'gamma_tests/third']),
                         sorted(re.findall(r'Entering test case "([^"]+)"', result.stdout)))
        self.assertIn('3 processes, 4 test cases', summary)
        self.assertIn('0 failed', summary)

        # A failing case fails the gate, by name, and the others still run.
        result, starts, summary = self.run_unit_gate('--jobs', '2', FAKE_FAIL='beta_tests/only')
        self.assertEqual(1, result.returncode)
        self.assertEqual(3, len(starts))
        self.assertIn('FAILED beta_tests (exit 201', result.stdout)
        self.assertIn('- FAILED `beta_tests`: exit 201', summary)
        self.assertIn('1 failed', summary)

        # Exit 0 is not enough: a process that ran fewer cases than the listing holds for it fails the gate.
        result, _, summary = self.run_unit_gate('--jobs', '2', FAKE_SHORT='gamma_tests/third')
        self.assertEqual(1, result.returncode)
        self.assertIn('- FAILED `gamma_tests`: ran 2 test cases, the listing holds 3', summary)

        # Nothing starts on a request the binary does not bear out, or on a listing that cannot be used.
        for arguments, env in ((['--exclude=delta_tests'], {}),
                               (['--exclude=alpha_tests/three'], {}),
                               (['--split=delta_tests'], {}),
                               (['--jobs', '0'], {}),
                               ([], {'FAKE_LIST_EXIT': '1'}),
                               ([], {'FAKE_LISTING': ''}),
                               ([], {'FAKE_LISTING': 'alpha_tests\n    one*\n'})):
            with self.subTest(arguments=arguments, env=env):
                result, starts, _ = self.run_unit_gate(*arguments, **env)
                self.assertEqual(1, result.returncode)
                self.assertEqual([], starts)
                self.assertNotEqual('', result.stderr)
        result, starts, _ = self.run_unit_gate('--binary', '/nonexistent/test_binary')
        self.assertEqual((1, []), (result.returncode, starts))

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
            'wallet_combineoutputs.py',
            'feature_dsl_dkg_isolation.py', 'feature_dsl_list_change.py',
            'rpc_help.py', 'tool_wallet.py',
            'wallet_multiwallet.py', 'rpc_getblockstats.py',
            'wallet_basic.py', 'wallet_backup.py', 'rpc_fundrawtransaction.py', 'wallet_send.py',
            'rpc_rawtransaction.py', 'wallet_balance.py', 'interface_bitcoin_cli.py', 'feature_coinstatsindex.py',
            'wallet_labels.py', 'wallet_orphanedreward.py', 'feature_txindex.py',
            'wallet_coinbase_category.py',
            'p2p_tx_download.py', 'wallet_avoidreuse.py', 'wallet_txn_doublespend.py', 'wallet_txn_clone.py',
            'rpc_createmultisig.py', 'mempool_reorg.py', 'interface_rest.py', 'mempool_spend_coinbase.py',
            'rpc_packages.py',
            'rpc_psbt.py', 'mempool_accept.py', 'p2p_invalid_tx.py',
            'rpc_blockchain.py', 'mining_basic.py', 'feature_minchainwork.py',
            'p2p_coinjoin_inputs.py',
        ]), sorted(name for name, _ in functional_gate.read_gate()))
        functional = run_script(step_text(self.checks, 'Functional tests (named gate)'))
        self.assertIn('python3 ci/dash/functional_gate.py order --shard "$shard" > "$gate" || exit 1\n', functional)
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
        self.assertIn("    if: needs.changes.outputs.code == 'true' && (" + NOT_A_FORK_PUSH + ")\n", fuzz)
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
