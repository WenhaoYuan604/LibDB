#!/usr/bin/env python3
"""GC-heavy runner integration tests use fake CMake/database, never hardware."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))
import run_ycsba_gc_heavy as runner
from summarize_ycsba_gc_heavy import inspect_trace, summarize, BYTE_FIELDS

DATABASE = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
args = dict(a[2:].split('=', 1) for a in sys.argv[1:])
assert args['cachesize'] == '20480'
assert args['thread'] == '16' and args['load-injectors'] == '8'
pmem = Path(args['pmem-dir'])
assert not list(pmem.iterdir())
(pmem / 'database').write_text('live database')
Path(os.environ['AUDIT']).write_text(json.dumps(args))
mode = os.environ.get('MOCK_MODE', '')
if mode == 'sleep': time.sleep(30)
if mode == 'fail': sys.exit(7)
text = []
for phase in ('load', 'warmup', 'precondition', 'measured'):
    text.append(f'Benchmark phase: begin {phase}')
    if phase in ('precondition', 'measured'):
        key = 'precondition' if phase == 'precondition' else 'test'
        ops = [line.split()[0] for line in Path(args[key]).read_text().splitlines()]
        window = dict(phase=phase, requests=len(ops), inserts=0, updates=ops.count('U'),
            reads=ops.count('R'), scans=0, kv_size_bytes=int(args['kvsize']), seconds=2.0,
            flush_bytes=409600, gc_bytes=8192, split_bytes=12288, merge_bytes=16384)
        if phase == 'precondition': window['gc_bytes'] = 999 * 4096
        if phase == 'measured':
            if mode == 'zero': window['updates'] = 0
            if mode == 'wrong_count': window['requests'] += 1000
            if mode == 'no_gc': window['gc_bytes'] = 0
            if mode == 'nonfinite': window['seconds'] = float('nan')
        if not (mode == 'missing' and phase == 'measured'):
            text.append('SSD window: ' + json.dumps(window))
    text.append(f'Benchmark phase: end {phase}')
if mode == 'incomplete': text.pop()
if mode == 'duplicate': text.extend(text)
text = '\n'.join(text) + '\n'
print(text, end='', flush=True)
if mode == 'mismatch': text = text.replace('409600', '413696')
Path(args['output']).write_text(text)
'''
CMAKE = r'''#!/usr/bin/env python3
import os, sys
from pathlib import Path
args = sys.argv[1:]
if os.environ.get('MOCK_MODE') == 'build_fail': sys.exit(8)
if '-B' in args:
    dest = Path(args[args.index('-B') + 1]); dest.mkdir(parents=True)
    assert '-DBUILD_TESTING=OFF' in args
    assert not any('DYNAMIC_FLUSH_ENABLED' in arg for arg in args)
else:
    dest = Path(args[args.index('--build') + 1])
    assert args[args.index('--target') + 1] == 'libdb'
    binary = dest / 'test'
    binary.write_text(Path(os.environ['MOCK_TEMPLATE']).read_text())
    binary.chmod(0o755)
'''


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='libdb gc heavy ')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.pmem = self.base / 'pmem'; self.pmem.mkdir()
        (self.pmem / 'keep').write_text('preserve')
        self.workload = self.base / 'workload'; self.workload.mkdir()
        (self.workload / 'warm.txt').write_text('I user1 value\n' * 1000)
        (self.workload / 'ycsba1.txt').write_text('R user1\nU user1 value\n' * 1000)
        # Deliberately not exactly 50/50: denominator must use actual updates.
        (self.workload / 'ycsba2.txt').write_text('R user1\n' * 600 + 'U user1 value\n' * 400)
        self.bin = self.base / 'bin'; self.bin.mkdir()
        cmake = self.bin / 'cmake'; cmake.write_text(CMAKE); cmake.chmod(0o755)
        template = self.base / 'database.py'; template.write_text(DATABASE)
        spdk = self.base / 'spdk'; spdk.mkdir()
        self.results = self.base / 'results'
        self.audit = self.base / 'audit.json'
        self.env = {key: value for key, value in os.environ.items() if not key.startswith(('LIBDB_', 'MOCK_'))}
        self.env.update(PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
            PYTHONPATH=str(REPO / 'tools'), SPDK_ROOT=str(spdk), PMEM_BASE=str(self.pmem),
            WORKLOAD_ROOT=str(self.workload), RESULT_BASE=str(self.results), USE_SUDO='0',
            SKIP_DRIVER_CHECK='1', LIBDB_ALLOW_DESTRUCTIVE='1', PCI_ADDR='0000:00:01.0',
            MOCK_TEMPLATE=str(template), AUDIT=str(self.audit), TIMEOUT_SECONDS='0',
            KILL_AFTER_SECONDS='1', THREADS='16', LOAD_INJECTORS='8', CACHE_SIZE_MB='20480',
            KV_SIZE='1024', TASKS_PER_THREAD='16')
        for key in ('WARM_FILE', 'PRECONDITION_FILE', 'TEST_FILE'):
            self.env.pop(key, None)

    def invoke(self, mode='', production_counts=False, **env):
        # Small fixtures only in the test harness. The public CLI has no size bypass.
        code = 'import run_ycsba_gc_heavy as r; '
        if not production_counts:
            code += "r.COUNTS={'load':1000,'precondition':2000,'measured':1000}; "
        code += 'raise SystemExit(r.main())'
        return subprocess.run([sys.executable, '-c', code], env=dict(self.env, MOCK_MODE=mode, **env),
                              capture_output=True, text=True, timeout=15)

    def folder(self):
        folders = list(self.results.iterdir()); self.assertEqual(len(folders), 1)
        return folders[0]

    def row(self):
        with (self.folder() / 'summary.csv').open() as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 1)
        return rows[0]

    def assert_clean(self):
        self.assertEqual([path.name for path in self.pmem.iterdir()], ['keep'])
        self.assertEqual((self.pmem / 'keep').read_text(), 'preserve')
        self.assertTrue((self.workload / 'ycsba2.txt').exists())

    def test_success_and_actual_updates_denominator(self):
        proc = self.invoke()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        row = self.row()
        self.assertEqual(row['status'], 'ok')
        self.assertEqual(float(row['throughput_ops_s']), 500)
        self.assertEqual(int(row['updates']), 400)
        self.assertEqual(int(row['logical_update_bytes']), 400 * 1024)
        self.assertAlmostEqual(float(row['ssd_write_amplification']),
                               (409600 + 8192 + 12288 + 16384) / (400 * 1024))
        self.assertEqual(row['gc_observed'], 'True')
        args = json.loads(self.audit.read_text())
        self.assertEqual(args['precondition'], str(self.workload / 'ycsba1.txt'))
        self.assertEqual(args['test'], str(self.workload / 'ycsba2.txt'))
        self.assertEqual(args['cachesize'], '20480')
        self.assertTrue((self.folder() / 'build' / 'test').is_file())
        self.assertIn('SSD WA=', proc.stdout)
        self.assert_clean()
        # Standalone summarization must reconstruct exactly the same result.
        res = subprocess.run([sys.executable, str(REPO / 'tools/summarize_ycsba_gc_heavy.py'), str(self.folder())], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(json.loads(res.stdout)['updates'], 400)

    def test_no_gc_is_explicit(self):
        proc = self.invoke('no_gc')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.row()['gc_observed'], 'False')
        self.assertIn('GC-heavy is NOT verified', proc.stdout)

    def test_invalid_results_never_report_success(self):
        for mode in ('fail', 'missing', 'wrong_count', 'zero', 'nonfinite', 'mismatch', 'incomplete', 'duplicate', 'build_fail'):
            with self.subTest(mode=mode):
                proc = self.invoke(mode, RESULT_BASE=str(self.base / mode))
                self.assertNotEqual(proc.returncode, 0, proc.stdout)
                self.assertNotIn('SSD WA=', proc.stdout)
                folder, = (self.base / mode).iterdir()
                status = json.loads((folder / 'status.json').read_text())
                self.assertEqual(status['status'], 'failed')
                with (folder / 'summary.csv').open() as stream: row, = csv.DictReader(stream)
                self.assertEqual(row['status'], 'failed')
                self.assertEqual(row['throughput_ops_s'], '')
                self.assert_clean()

    def test_timeout_cleanup(self):
        proc = self.invoke('sleep', TIMEOUT_SECONDS='1')
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(self.row()['status'], 'timeout')
        self.assert_clean()

    def test_default_counts_and_bad_inputs_rejected_before_build(self):
        self.assertEqual(runner.COUNTS, dict(load=100000000, precondition=150000000, measured=100000000))
        proc = self.invoke(production_counts=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertFalse(self.results.exists())
        self.assertFalse(self.audit.exists())
        (self.workload / 'ycsba2.txt').write_text('S user1 5\n' * 1000)
        proc = self.invoke()
        self.assertNotEqual(proc.returncode, 0)
        self.assertFalse(self.results.exists())

    def test_final_size_cannot_be_short(self):
        (self.workload / 'ycsba2.txt').write_text('R user1\nU user1 value\n' * 499)
        proc = self.invoke()
        self.assertNotEqual(proc.returncode, 0)
        self.assertFalse(self.audit.exists())

    def test_trace_validation_edges(self):
        path = self.base / 'trace'
        for bad in ('R user1 extra\n', 'U user1\n', 'U user18446744073709551616 value\n',
                    'U user1 ' + 'x' * 100 + '\n', 'I user1 value\n', 'R user1\n\n'):
            with self.subTest(bad=bad):
                path.write_text(bad * 1000)
                with self.assertRaises(ValueError): inspect_trace(path, 'measured', 1000)
        path.write_text(('R user1\r\nU user1 value\r\n' * 500).rstrip('\r\n'))
        self.assertEqual(inspect_trace(path, 'measured', 1000)['updates'], 500)


if __name__ == '__main__':
    unittest.main()
