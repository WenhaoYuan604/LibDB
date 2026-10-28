#!/usr/bin/env python3
"""No-device integration tests: mock CMake/database, real stdbuf and process control."""
import csv
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))
import run_ycsba_dynamic_flush as runner
SCRIPT = REPO / 'run_ycsba_dynamic_flush.sh'

DATABASE = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
enabled = @ENABLED@
args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:])
pmem = Path(args['pmem-dir'])
assert not list(pmem.iterdir()), 'database is not fresh'
(pmem / 'database').write_text('private')
with open(os.environ['AUDIT'], 'a') as stream:
    stream.write(json.dumps(dict(event='database', enabled=enabled, args=args, pid=os.getpid())) + '\n')
print('LIVE_STDOUT', flush=True)
print('LIVE_STDERR', file=sys.stderr, flush=True)
mode = os.environ.get('MOCK_MODE', '')
if mode == 'sleep': time.sleep(30)
if mode == 'gate' and not enabled:
    while not Path(os.environ['RELEASE']).exists(): time.sleep(.02)
if mode == 'cap_fail':
    print('WBF hard limit: file_bytes=17179869184 requested_bytes=1073741824 hard_limit_bytes=17179869184 limit_rejections=1', file=sys.stderr, flush=True)
    sys.exit(1)
if mode == 'fail': sys.exit(7)
if mode == 'incomplete': sys.exit(0)
rate = 200 if enabled else 100
seconds = 1000 / rate
actual = 1 - enabled if mode == 'wrong_mode' else enabled
header = (f'NVM flush: enabled={actual} limit_bytes=17179869184 poll_ns=100000000 '
          'occupancy=unreclaimed_slots phase=workload_only soft_limit=1\n')
stat = (f'nvm_used_slot_bytes=1234 nvm_sampled_peak_bytes={5678 if enabled else 0} '
        f'nvm_pressure_changes={2 if enabled else 0} nvm_sampled={actual}\n')
print('1000 ops , time is 1.000000 , throughput is 1000.000000')
print('500 ops , time is 1.000000 , throughput is 500.000000')
print('start to read file ' + args['test'])
print('read file ' + args['test'] + ' 1000 request done')
print(f'1000 ops , time is {seconds:.6f} , throughput is {rate:.6f}')
print(header + stat, end='', flush=True)
Path(args['output']).write_text(header +
    'nvm_used_slot_bytes=0 nvm_sampled_peak_bytes=0 nvm_pressure_changes=0 nvm_sampled=0\n'
    'test:total 1000 ops Insert 1000 Update 0 Get 0 Scan 0 time 1.000000s\n'
    'test:total 500 ops Insert 0 Update 0 Get 500 Scan 0 time 1.000000s\n' +
    f'test:total 1000 ops Insert 0 Update 500 Get 500 Scan 0 time {seconds:.6f}s\n' + header + stat)
'''
CMAKE = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
args = sys.argv[1:]
building = '--build' in args
directory = Path(args[args.index('--build' if building else '-B') + 1])
mode = directory.name
step = 'build' if building else 'configure'
with open(os.environ['AUDIT'], 'a') as stream:
    stream.write(json.dumps(dict(event=step, mode=mode, args=args, pid=os.getpid())) + '\n')
print(f'MOCK_CMAKE {step} {mode}', flush=True)
if os.environ.get('MOCK_BUILD_SLEEP') == f'{step}_{mode}': time.sleep(30)
if os.environ.get('MOCK_BUILD_FAIL') == f'{step}_{mode}': sys.exit(9)
if not building:
    assert '-DBUILD_TESTING=OFF' in args
    flag = next(arg for arg in args if arg.startswith('-DCMAKE_C_FLAGS='))
    enabled = int(flag.rsplit('=', 1)[1])
    assert enabled == (mode == 'on')
    directory.mkdir(parents=True)
    (directory / 'enabled').write_text(str(enabled))
else:
    assert args[args.index('--target') + 1] == 'libdb'
    enabled = (directory / 'enabled').read_text()
    template = Path(os.environ['MOCK_DATABASE_TEMPLATE']).read_text()
    binary = directory / 'test'
    binary.write_text(template.replace('@ENABLED@', enabled))
    binary.chmod(0o755)
'''


class DynamicFlushRunnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='libdb dynamic test ')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.pmem = self.base / 'pmem'; self.pmem.mkdir()
        (self.pmem / 'keep').write_text('unrelated')
        self.uniform = self.base / 'workloads' / 'uniform'; self.uniform.mkdir(parents=True)
        (self.uniform / 'warm.txt').write_text('I user1 value\n' * 1000)
        (self.uniform / 'ycsba.txt').write_text('R user1\nU user1 value\n' * 500)
        self.bin = self.base / 'bin'; self.bin.mkdir()
        cmake = self.bin / 'cmake'; cmake.write_text(CMAKE); cmake.chmod(0o755)
        template = self.base / 'database.py'; template.write_text(DATABASE)
        spdk = self.base / 'spdk'; spdk.mkdir()
        self.audit = self.base / 'audit.jsonl'
        self.result = self.base / 'results'
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(('LIBDB_', 'UNIFORM_', 'ZIPFIAN_', 'MOCK_'))}
        self.env.update(PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
            SPDK_ROOT=str(spdk), PMEM_BASE=str(self.pmem), RESULT_BASE=str(self.result),
            WORKLOAD_ROOT=str(self.uniform.parent), PCI_ADDR='0000:00:01.0',
            USE_SUDO='0', SKIP_DRIVER_CHECK='1', LIBDB_ALLOW_DESTRUCTIVE='1',
            AUDIT=str(self.audit), MOCK_DATABASE_TEMPLATE=str(template),
            TIMEOUT_SECONDS='0', KILL_AFTER_SECONDS='1', BUILD_JOBS='6', LIBDB_DYNAMIC_FLUSH_MODES='off,on',
            THREADS='16', LOAD_INJECTORS='8', KV_SIZE='1024', KV_COUNT='100000000',
            CACHE_SIZE_MB='20000', TASKS_PER_THREAD='16', BOUNDARY_SIZE='1000')

    def run_script(self, **env):
        return subprocess.run([str(SCRIPT)], env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=15)

    def events(self):
        return [json.loads(line) for line in self.audit.read_text().splitlines()] if self.audit.exists() else []

    def folder(self):
        folders = list(self.result.iterdir()); self.assertEqual(len(folders), 1)
        return folders[0]

    def rows(self):
        with (self.folder() / 'summary.csv').open() as stream:
            return list(csv.DictReader(stream))

    def clean(self):
        self.assertEqual([p.name for p in self.pmem.iterdir()], ['keep'])
        self.assertEqual((self.pmem / 'keep').read_text(), 'unrelated')
        self.assertEqual((self.uniform / 'warm.txt').read_text(), 'I user1 value\n' * 1000)

    def wait_database(self, proc):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            events = [e for e in self.events() if e['event'] == 'database']
            if events: return events[0]
            if proc.poll() is not None: break
            time.sleep(.02)
        self.fail('database never started')

    def test_two_builds_then_two_fresh_runs_and_comparison(self):
        result = self.run_script(REPEATS='99', BINARY='/ignored/nonexistent')
        self.assertEqual(result.returncode, 0, result.stderr)
        events = self.events()
        self.assertEqual([e['event'] for e in events], ['configure', 'build', 'configure', 'build', 'database', 'database'])
        self.assertEqual([e.get('mode') for e in events[:4]], ['off', 'off', 'on', 'on'])
        self.assertEqual([e['enabled'] for e in events[4:]], [0, 1])
        a, b = [e['args'] for e in events[4:]]
        for key in set(a) - {'pmem-dir', 'output'}:
            self.assertEqual(a[key], b[key])
        self.assertEqual(a['thread'], '16'); self.assertEqual(a['tasks-per-thread'], '16')
        self.assertNotEqual(a['pmem-dir'], b['pmem-dir'])
        rows = self.rows()
        self.assertEqual([r['status'] for r in rows], ['ok', 'ok'])
        self.assertEqual([r['mode'] for r in rows], ['off', 'on'])
        self.assertEqual([r['throughput_ops_s'] for r in rows], ['100.000000', '200.000000'])
        self.assertEqual(rows[1]['nvm_sampled_peak_bytes'], '5678')
        self.assertEqual(rows[1]['nvm_pressure_changes'], '2')
        self.assertIn('ON relative to OFF: +100.00%', result.stdout)
        for mode in ('off', 'on'):
            command = json.loads((self.folder() / f'{mode}_uniform_A.command.json').read_text())
            self.assertEqual(command[1:3], ['-oL', '-eL'])
            self.assertEqual(Path(command[0]).name, 'stdbuf')
            self.assertTrue((self.folder() / 'build' / mode / 'test').is_file())
            log = (self.folder() / f'{mode}_uniform_A.log').read_text()
            self.assertIn('LIVE_STDOUT', log); self.assertIn('LIVE_STDERR', log)
        self.assertIn('MOCK_CMAKE', result.stdout)
        self.assertEqual(json.loads((self.folder() / 'status.json').read_text())['status'], 'ok')
        self.clean()

    def test_stdout_stderr_visible_while_database_is_still_running(self):
        release = self.base / 'release'
        proc = subprocess.Popen([str(SCRIPT)], env=dict(self.env, MOCK_MODE='gate', RELEASE=str(release)),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            seen = b''
            with selectors.DefaultSelector() as selector:
                selector.register(proc.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + 5
                while b'LIVE_STDERR' not in seen and time.monotonic() < deadline:
                    for key, _ in selector.select(.1):
                        chunk = os.read(key.fd, 65536)
                        self.assertTrue(chunk, seen.decode())
                        seen += chunk
            self.assertIn(b'LIVE_STDOUT', seen); self.assertIn(b'LIVE_STDERR', seen)
            self.assertIsNone(proc.poll())
            pid = self.wait_database(proc)['pid']; os.kill(pid, 0)
            log = (self.folder() / 'off_uniform_A.log').read_bytes()
            self.assertIn(b'LIVE_STDOUT', log); self.assertIn(b'LIVE_STDERR', log)
            release.write_text('continue')
            output, _ = proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 0, output.decode())
            self.clean()
        finally:
            if proc.poll() is None: proc.terminate(); proc.communicate(timeout=5)
            proc.stdout.close()

    def test_source_default_is_off_then_on(self):
        env = dict(self.env)
        env.pop('LIBDB_DYNAMIC_FLUSH_MODES')
        result = subprocess.run([str(SCRIPT)], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row['mode'] for row in self.rows()], ['off', 'on'])
        self.clean()

    def test_single_on_mode(self):
        result = self.run_script(LIBDB_DYNAMIC_FLUSH_MODES='on')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row['mode'] for row in self.rows()], ['on'])
        self.assertIn('single mode', (self.folder() / 'comparison.txt').read_text())
        self.clean()

    def test_capacity_rejection_is_reported_even_without_final_stats(self):
        result = self.run_script(MOCK_MODE='cap_fail')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'capacity_limit')
        self.assertEqual(self.rows()[0]['wbf_limit_rejections'], '1')
        self.assertEqual(self.rows()[0]['wbf_capacity_limit_hit'], '1')
        self.clean()

    def test_build_failure_prevents_all_database_runs(self):
        result = self.run_script(MOCK_BUILD_FAIL='build_on')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(e['event'] == 'database' for e in self.events()))
        self.assertEqual(self.rows(), [])
        status = json.loads((self.folder() / 'status.json').read_text())
        self.assertEqual(status['stage'], 'build_on'); self.assertEqual(status['status'], 'failed')
        self.assertTrue((self.folder() / 'build_on.log').exists())
        self.clean()

    def test_build_timeout_stops_build_before_pmem_preparation(self):
        result = self.run_script(MOCK_BUILD_SLEEP='build_off', TIMEOUT_SECONDS='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads((self.folder() / 'status.json').read_text())['status'], 'timeout')
        self.assertFalse(any(e['event'] == 'database' for e in self.events()))
        with self.assertRaises(ProcessLookupError): os.kill(self.events()[-1]['pid'], 0)
        self.clean()

    def test_runtime_failure_stops_before_on(self):
        result = self.run_script(MOCK_MODE='fail')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.rows()), 1); self.assertEqual(self.rows()[0]['status'], 'failed')
        self.assertIn('exit=7', self.rows()[0]['error']); self.clean()

    def test_mode_mismatch_is_not_reported_as_success(self):
        result = self.run_script(MOCK_MODE='wrong_mode')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'invalid_result')
        self.assertIn('mode mismatch', self.rows()[0]['error'])
        self.assertEqual(len(self.rows()), 1); self.clean()

    def test_missing_metrics(self):
        result = self.run_script(MOCK_MODE='incomplete')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'invalid_result'); self.clean()

    def test_runtime_timeout_stops_child_before_cleanup(self):
        result = self.run_script(MOCK_MODE='sleep', TIMEOUT_SECONDS='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'timeout')
        pid = next(e['pid'] for e in self.events() if e['event'] == 'database')
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        self.clean()

    def test_interrupt_stops_child_before_cleanup(self):
        proc = subprocess.Popen([str(SCRIPT)], env=dict(self.env, MOCK_MODE='sleep'),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            pid = self.wait_database(proc)['pid']
            proc.send_signal(signal.SIGTERM); proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 143)
            self.assertEqual(self.rows()[0]['status'], 'interrupted')
            with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
            self.clean()
        finally:
            if proc.poll() is None: proc.terminate(); proc.communicate(timeout=5)

    def test_invalid_inputs_and_explicit_disable_do_not_build(self):
        for overrides in ({'LIBDB_ALLOW_DESTRUCTIVE': '0'}, {'UNIFORM_YCSBA_FILE': '/no/such/file'},
                          {'BUILD_JOBS': '0'}, {'SKIP_DRIVER_CHECK': '0', 'PCI_ADDR': 'ffff:ff:1f.7'}):
            with self.subTest(overrides=overrides):
                result = self.run_script(**overrides)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.audit.exists()); self.assertFalse(self.result.exists())
                self.clean()
        (self.uniform / 'ycsba.txt').write_text('R user1\n')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('positive multiple of 1000', result.stderr)
        self.assertFalse(self.audit.exists())

    def test_overrides_and_only_uniform_a_files_required(self):
        result = self.run_script(WORKLOAD_ROOT='/missing', THREADS='2', LOAD_INJECTORS='1',
            KV_SIZE='512', CACHE_SIZE_MB='128', TASKS_PER_THREAD='2', BOUNDARY_SIZE='50', BUILD_JOBS='2',
            UNIFORM_WARM_FILE=str(self.uniform / 'warm.txt'),
            UNIFORM_YCSBA_FILE=str(self.uniform / 'ycsba.txt'))
        self.assertEqual(result.returncode, 0, result.stderr)
        for row in self.rows():
            self.assertEqual(row['workers'], '2'); self.assertEqual(row['tasks_per_thread'], '2')
            self.assertEqual(row['kv_size_bytes'], '512')
        for e in self.events():
            if e['event'] == 'build': self.assertEqual(e['args'][-2:], ['--parallel', '2'])
        self.clean()

    def test_unstopped_database_preserves_private_pmem(self):
        original = runner.execute
        def execute(command, log, c):
            if any(arg.startswith('--pmem-dir=') for arg in command):
                c['_PROCESS_UNSTOPPED'] = True
                raise RuntimeError('mock process could not be stopped')
            return original(command, log, c)
        with mock.patch.dict(os.environ, self.env, clear=True), mock.patch.object(runner, 'execute', side_effect=execute):
            c = runner.configuration()
            with self.assertRaisesRegex(RuntimeError, 'could not be stopped'):
                runner.run(c)
        self.assertTrue(c['PMEM_DIR'].is_dir())
        self.assertEqual((self.pmem / 'keep').read_text(), 'unrelated')
        self.assertEqual(len(self.rows()), 1)

    def test_help_does_not_require_devices_or_inputs(self):
        result = subprocess.run([str(SCRIPT), '--help'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertIn('source MODES setting (OFF then ON)', result.stdout)
        self.assertFalse(self.result.exists())


class CapacityMetricsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / 'run.log'
        self.result = Path(self.temp.name) / 'result.txt'
        self.header = ('NVM flush: enabled=1 limit_bytes=16106127360 poll_ns=100000000 '
            'occupancy=file_budget_minus_reusable phase=workload_only soft_limit=0 '
            'low_bytes=15032385536 hard_limit_bytes=17179869184\n')
        self.stat = ('nvm_used_slot_bytes=1234 nvm_sampled_peak_bytes=5678 '
            'nvm_pressure_changes=2 nvm_sampled=1\n')
        self.budget = ('wbf_file_count=16 wbf_file_bytes=17179869184 wbf_file_peak_bytes=17179869184 '
            'wbf_reusable_slot_bytes=2147483648 nvm_pressure_bytes=15032385536 '
            'nvm_pressure_sampled_peak_bytes=16106127360 wbf_limit_rejections=0\n')

    def parse(self, budget=None, header=None):
        text = (self.header if header is None else header) + self.stat + (self.budget if budget is None else budget)
        self.log.write_text(text); self.result.write_text(text)
        return runner.nvm_metrics(self.log, self.result, 1)

    def test_new_metrics(self):
        metrics = self.parse()
        self.assertEqual(metrics['nvm_low_bytes'], 14 * 1024**3)
        self.assertEqual(metrics['nvm_limit_bytes'], 15 * 1024**3)
        self.assertEqual(metrics['nvm_hard_limit_bytes'], 16 * 1024**3)
        self.assertEqual(metrics['wbf_capacity_verified'], 1)
        self.assertEqual(metrics['wbf_capacity_limit_hit'], 0)
        self.assertEqual(metrics['nvm_sampled_peak_bytes'], 5678)
        self.assertEqual(metrics['nvm_pressure_sampled_peak_bytes'], 15 * 1024**3)

    def test_legacy_is_not_hard_cap_verified(self):
        metrics = self.parse('', 'NVM flush: enabled=1 limit_bytes=17179869184 poll_ns=100000000 '
            'occupancy=unreclaimed_slots phase=workload_only soft_limit=1\n')
        self.assertEqual(metrics['wbf_capacity_verified'], 0)
        self.assertIsNone(metrics['nvm_hard_limit_bytes'])

    def test_new_format_requires_capacity(self):
        with self.assertRaisesRegex(ValueError, 'missing WBF'): self.parse('')

    def test_rejects_excess_capacity_or_inconsistent_pressure(self):
        with self.assertRaisesRegex(ValueError, 'exceeded WBF'):
            self.parse(self.budget.replace('wbf_file_peak_bytes=17179869184', 'wbf_file_peak_bytes=20401094656'))
        with self.assertRaisesRegex(ValueError, 'invalid or exceeded'):
            self.parse(self.budget.replace('nvm_pressure_bytes=15032385536', 'nvm_pressure_bytes=1'))

    def test_limit_rejections_are_visible(self):
        metrics = self.parse(self.budget.replace('wbf_limit_rejections=0', 'wbf_limit_rejections=3'))
        self.assertEqual(metrics['wbf_capacity_limit_hit'], 1)
        self.assertEqual(metrics['wbf_limit_rejections'], 3)

    def test_streams_must_match(self):
        self.parse()
        self.result.write_text(self.result.read_text().replace('nvm_pressure_changes=2', 'nvm_pressure_changes=3'))
        with self.assertRaisesRegex(ValueError, 'disagree'):
            runner.nvm_metrics(self.log, self.result, 1)


if __name__ == '__main__':
    unittest.main()
