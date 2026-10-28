#!/usr/bin/env python3
"""Integration tests use only temporary files and a mock database, never hardware."""
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest
from unittest import mock
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_ycsbe_latency_task2 as runner

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'run_ycsbe_latency_task2.sh'
MOCK = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:])
pmem = Path(args['pmem-dir'])
assert not list(pmem.iterdir()), 'database is not fresh'
(pmem / 'database').write_text('private data')
audit = Path(os.environ['AUDIT'])
prior = audit.read_text().splitlines() if audit.exists() else []
with audit.open('a') as stream:
    stream.write(json.dumps(dict(args, pid=os.getpid())) + '\n')
mode = os.environ.get('MOCK_MODE', '')
if mode == 'fail': sys.exit(7)
if mode == 'sleep': time.sleep(30)
if mode == 'incomplete': sys.exit(0)
rate = 100 * (len(prior) + 1)
seconds = 1000 / rate
scale = len(prior) + 1
def hist(scale, measured=False):
    for cutoff, value in ((50,10), (95,20), (99,30), (99.9,40)):
        if measured and mode == 'missing' and cutoff == 99: continue
        print(f'{cutoff:9.5f}% : {value * scale:9.3f}us')
    print(f'最大延迟: {50 * scale:.3f}us')
    print(f'平均延迟: {15 * scale:.3f}us')
print('1000 ops , time is 1.000000 , throughput is 1000.000000')
hist(100)
print('500 ops , time is 1.000000 , throughput is 500.000000')
hist(200)
print('start to read file ' + args['test'])
print('read file ' + args['test'] + ' 1000 request done')
print(f'1000 ops , time is {seconds:.6f} , throughput is {rate:.6f}')
if mode != 'no_hist': hist(scale, True)
if mode == 'duplicate': print('99.00000% : 30.000us')
if mode == 'nan': print('平均延迟: nanus')
if mode == 'bad_order': print('98.00000% : 1000000.000us')
Path(args['output']).write_text(
    'test:total 1000 ops Insert 1000 Update 0 Get 0 Scan 0 time 1.000000s\n'
    'test:total 500 ops Insert 0 Update 0 Get 500 Scan 0 time 1.000000s\n'
    f'test:total 1000 ops Insert 50 Update 0 Get 0 Scan 950 time {seconds:.6f}s\n')
'''

class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='libdb-ycsba-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.pmem = self.base / 'pmem'; self.pmem.mkdir()
        (self.pmem / 'keep').write_text('unrelated')
        self.work = self.base / 'workloads'
        for dist in ('uniform',):
            folder = self.work / dist; folder.mkdir(parents=True)
            (folder / 'warm.txt').write_text('I user1 value\n' * 1000)
            (folder / 'ycsbe.txt').write_text('I user1 value\n' * 50 + 'S user1 10\n' * 950)
        binary = self.base / 'mock.py'; binary.write_text(MOCK); binary.chmod(0o755)
        self.result = self.base / 'results'
        self.audit = self.base / 'audit.jsonl'
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('LIBDB_', 'UNIFORM_', 'ZIPFIAN_'))}
        self.env.update(BINARY=str(binary), PMEM_BASE=str(self.pmem), RESULT_BASE=str(self.result),
            WORKLOAD_ROOT=str(self.work), PCI_ADDR='0000:00:01.0', USE_SUDO='0',
            SKIP_DRIVER_CHECK='1', LIBDB_ALLOW_DESTRUCTIVE='1', AUDIT=str(self.audit),
            REPEATS='3', TIMEOUT_SECONDS='0', KILL_AFTER_SECONDS='1')

    def run_script(self, **env):
        return subprocess.run([str(SCRIPT)], env=dict(self.env, **env), capture_output=True, text=True, timeout=15)

    def rows(self):
        folders = list(self.result.iterdir()); self.assertEqual(len(folders), 1)
        self.folder = folders[0]
        with (self.folder / 'summary.csv').open() as stream:
            return list(csv.DictReader(stream))

    def clean(self):
        self.assertEqual([p.name for p in self.pmem.iterdir()], ['keep'])
        self.assertEqual((self.work / 'uniform/warm.txt').read_text(), 'I user1 value\n' * 1000)

    def test_default_configuration_without_real_devices(self):
        env = dict(self.env)
        for key in ('PCI_ADDR', 'PMEM_BASE', 'LIBDB_ALLOW_DESTRUCTIVE'):
            env.pop(key)
        original = runner.existing
        def paths(name, default, directory=False):
            if name == 'PMEM_BASE':
                self.assertEqual(default, '/mnt/whypmem')
                return self.pmem
            return original(name, default, directory)
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(runner, 'existing', side_effect=paths):
            config = runner.configuration()
        self.assertEqual(config['PCI_ADDR'], '0000:5c:00.0')
        self.assertEqual(config['LIBDB_ALLOW_DESTRUCTIVE'], '1')
        self.assertEqual(config['PMEM_BASE'], self.pmem)
        self.assertFalse(self.audit.exists())

    def test_explicit_configuration_and_disable(self):
        with mock.patch.dict(os.environ, self.env, clear=True):
            config = runner.configuration()
        self.assertEqual(config['PCI_ADDR'], self.env['PCI_ADDR'])
        self.assertEqual(config['PMEM_BASE'], self.pmem)
        result = self.run_script(LIBDB_ALLOW_DESTRUCTIVE='0')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.audit.exists())

    def test_program_failure(self):
        result = self.run_script(MOCK_MODE='fail')
        self.assertNotEqual(result.returncode, 0)
        rows = self.rows(); self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['status'], 'failed')
        self.assertIn('exit=7', rows[0]['error']); self.clean()

    def test_missing_statistics(self):
        result = self.run_script(MOCK_MODE='incomplete')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'invalid_result'); self.clean()

    def test_timeout(self):
        result = self.run_script(MOCK_MODE='sleep', TIMEOUT_SECONDS='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'timeout')
        pid = json.loads(self.audit.read_text().splitlines()[0])['pid']
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        self.clean()

    def test_interrupt_stops_child_before_cleanup(self):
        proc = subprocess.Popen([str(SCRIPT)], env=dict(self.env, MOCK_MODE='sleep'),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not self.audit.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertTrue(self.audit.exists())
            proc.send_signal(signal.SIGTERM)
            proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 143)
            self.assertEqual(self.rows()[0]['status'], 'interrupted')
            pid = json.loads(self.audit.read_text().splitlines()[0])['pid']
            with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
            self.clean()
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate()

    def test_missing_input_and_nonmultiple_counts(self):
        result = self.run_script(UNIFORM_WARM_FILE=str(self.base / 'missing'))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.audit.exists()); self.assertFalse(self.result.exists())
        (self.work / 'uniform/warm.txt').write_text('I user1 value\n')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('positive multiple of 1000', result.stderr)
        self.assertFalse(self.audit.exists())

    def test_log_write_failure_stops_process(self):
        log = mock.MagicMock()
        log.open.return_value.__enter__.return_value.write.side_effect = OSError('disk full')
        code = 'import os, time; print(os.getpid(), flush=True); time.sleep(30)'
        processes = []
        original = subprocess.Popen
        def capture(*args, **kwargs):
            proc = original(*args, **kwargs); processes.append(proc); return proc
        with mock.patch.object(runner.subprocess, 'Popen', side_effect=capture):
            with self.assertRaisesRegex(OSError, 'disk full'):
                runner.execute([sys.executable, '-c', code], log,
                    {'TIMEOUT_SECONDS': 5, 'KILL_AFTER_SECONDS': 1, 'ROOT_COMMAND': []})
        self.assertIsNotNone(processes[0].poll())

    def test_driver_check_enabled_by_default(self):
        result = self.run_script(SKIP_DRIVER_CHECK='0', PCI_ADDR='ffff:ff:1f.7')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('already bound', result.stderr)
        self.assertFalse(self.audit.exists())
    def test_three_fresh_e_runs_fixed_task2_and_latency(self):
        result = self.run_script(REPEATS='99', TASKS_PER_THREAD='16')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows()
        self.assertEqual([(r['round'], r['distribution'], r['workload']) for r in rows],
                         [(str(i), 'uniform', 'E') for i in (1,2,3)])
        audit = [json.loads(line) for line in self.audit.read_text().splitlines()]
        self.assertEqual(len(audit), 3)
        for i, row in enumerate(rows, 1):
            self.assertEqual(row['status'], 'ok')
            self.assertEqual(row['tasks_per_thread'], '2')
            for name, value in [('avg_latency_us',15), ('p50_us',10), ('p95_us',20),
                                ('p99_us',30), ('p999_us',40), ('max_latency_us',50)]:
                self.assertEqual(float(row[name]), value * i)
            self.assertEqual(audit[i-1]['tasks-per-thread'], '2')
            self.assertTrue(audit[i-1]['test'].endswith('/uniform/ycsbe.txt'))
        with (self.folder / 'aggregate.csv').open() as stream:
            aggregate = list(csv.DictReader(stream))[0]
        self.assertEqual(aggregate['successful_runs'], '3')
        self.assertEqual(float(aggregate['p99_us_mean']), 60)
        self.assertEqual(float(aggregate['p99_us_stddev']), 30)
        self.assertEqual(float(aggregate['throughput_ops_s_mean']), 200)
        self.clean()

    def test_bad_histograms_fail_without_fallback(self):
        import shutil
        for mode in ('missing', 'no_hist', 'duplicate', 'bad_order', 'nan'):
            with self.subTest(mode=mode):
                result = self.run_script(MOCK_MODE=mode)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                rows = self.rows(); self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]['status'], 'invalid_result')
                self.assertEqual(rows[0]['p99_us'], '')
                self.clean(); shutil.rmtree(self.result); self.audit.unlink()

    def test_missing_e_is_checked_before_running(self):
        (self.work / 'uniform/ycsbe.txt').unlink()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.audit.exists()); self.assertFalse(self.result.exists())
        self.clean()

    def test_file_overrides(self):
        result = self.run_script(WORKLOAD_ROOT='/missing',
            UNIFORM_WARM_FILE=str(self.work/'uniform/warm.txt'),
            UNIFORM_YCSBE_FILE=str(self.work/'uniform/ycsbe.txt'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.rows()), 3); self.clean()

if __name__ == '__main__': unittest.main()
