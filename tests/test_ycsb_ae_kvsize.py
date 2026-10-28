#!/usr/bin/env python3
"""No-device matrix integration tests with fake CMake and database executables."""
import csv
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))
from summarize_ycsb_ae_kvsize import PROFILES, GIB, aggregate, measured_result
SCRIPT = REPO / 'run_ycsb_ae_kvsize.sh'

CMAKE = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
building = '--build' in args
directory = Path(args[args.index('--build' if building else '-B') + 1])
step = 'build' if building else 'configure'
with open(os.environ['AUDIT'], 'a') as stream:
    stream.write(json.dumps(dict(event=step, kv=directory.name, args=args)) + '\n')
if os.environ.get('MOCK_BUILD_FAIL') == directory.name: sys.exit(9)
if not building:
    assert '-DBUILD_TESTING=OFF' in args
    flag = next(arg for arg in args if arg.startswith('-DCMAKE_C_FLAGS='))
    flags = dict(s[2:].split('=', 1) for s in flag.split('=', 1)[1].split())
    directory.mkdir(parents=True)
    (directory / 'flags.json').write_text(json.dumps(flags))
else:
    assert args[args.index('--target') + 1] == 'libdb'
    binary = directory / 'test'
    binary.write_text(Path(os.environ['MOCK_TEMPLATE']).read_text())
    binary.chmod(0o755)
'''
DATABASE = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:])
flags = json.loads(Path(__file__).with_name('flags.json').read_text())
value = lambda name: int(flags[name].rstrip('UL'))
pmem = Path(args['pmem-dir'])
assert not list(pmem.iterdir()), 'not a fresh database'
(pmem / 'state').write_text('loaded')
with open(os.environ['AUDIT'], 'a') as stream:
    stream.write(json.dumps(dict(event='database', args=args, flags=flags, pid=os.getpid())) + '\n')
mode = os.environ.get('MOCK_MODE', '')
is_e = 'precondition' in args
high, low, hard = (value(n) for n in ('LIBDB_NVM_FLUSH_HIGH_BYTES', 'LIBDB_NVM_FLUSH_LOW_BYTES', 'LIBDB_WBF_CAPACITY_LIMIT_BYTES'))
if mode == 'wrong_threshold': high -= 1
header = (f'NVM flush: enabled={value("LIBDB_DYNAMIC_FLUSH_ENABLED")} limit_bytes={high} poll_ns=100000000 '
          f'occupancy=file_budget_minus_reusable phase=workload_only soft_limit=0 low_bytes={low} hard_limit_bytes={hard}')
output = Path(args['output']).open('w')
def emit(line, target='both'):
    if target != 'result': print(line, flush=True)
    if target != 'log': print(line, file=output, flush=True)
if mode != 'missing_flush':
    flush_blocks = value('LIBDB_FLUSH_BLOCK_COUNT')
    if mode == 'wrong_flush': flush_blocks += 1
    emit(f'LIBDB_FLUSH_BLOCK_COUNT={flush_blocks}', 'result')
    if mode == 'duplicate_flush': emit(f'LIBDB_FLUSH_BLOCK_COUNT={flush_blocks}', 'result')
def stats(peak):
    emit(header)
    emit(f'nvm_used_slot_bytes=7 nvm_sampled_peak_bytes={peak} nvm_pressure_changes=0 nvm_sampled=1')
    emit('wbf_file_count=1 wbf_file_bytes=1073741824 wbf_file_peak_bytes=1073741824 '
         'wbf_reusable_slot_bytes=1073741817 nvm_pressure_bytes=7 '
         f'nvm_pressure_sampled_peak_bytes={peak} wbf_limit_rejections=0')
def phase(name, count, ops, trace=None, rate=1000, peak=22):
    emit('Benchmark phase: begin ' + name)
    if trace:
        emit('start to read file ' + trace, 'log')
        emit(f'read file {trace} {count} request done', 'log')
    if name == 'precondition':
        assert (pmem / 'state').read_text() == 'loaded'
        if mode == 'pre_fail': sys.exit(7)
        (pmem / 'state').write_text('after_uniform_A')
    if name == 'measured':
        if mode == 'sleep': time.sleep(30)
        if mode == 'fail': sys.exit(7)
    if name == 'measured' and is_e:
        assert (pmem / 'state').read_text() == 'after_uniform_A'
    seconds = count / rate
    emit(f'{count} ops , time is {seconds:.6f} , throughput is {rate:.6f}', 'log')
    if mode == 'duplicate_rate' and name == 'measured':
        emit(f'{count} ops , time is {seconds:.6f} , throughput is {rate:.6f}', 'log')
    if mode == 'wrong_ops' and name == 'measured': ops = (count, 0, 0, 0)
    if not (mode == 'missing_stats' and name == 'measured'):
        emit(f'test:total {count} ops Insert {ops[0]} Update {ops[1]} Get {ops[2]} Scan {ops[3]} time {seconds:.6f}s', 'result')
    if trace: stats(peak)
    if not ((mode == 'missing_pre_end' and name == 'precondition') or
            (mode == 'missing_end' and name == 'measured')):
        emit('Benchmark phase: end ' + name)
phase('load', 2000, (2000,0,0,0))
phase('warmup', 1000, (0,0,1000,0))
if is_e: phase('precondition', 1000, (0,500,500,0), args['precondition'], rate=800, peak=999)
if mode == 'cap_fail':
    emit(f'WBF hard limit: file_bytes={hard} requested_bytes=1073741824 hard_limit_bytes={hard} limit_rejections=1', 'log')
    sys.exit(1)
phase('measured', 3000 if is_e else 1000, (150,0,0,2850) if is_e else (0,500,500,0),
      args['test'], rate=200 if is_e else 100)
stats(22)
output.close()
'''


class MatrixTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='libdb ae matrix ')
        self.addCleanup(self.temp.cleanup)
        base = self.base = Path(self.temp.name)
        self.pmem = base / 'pmem'; self.pmem.mkdir()
        (self.pmem / 'keep').write_text('unrelated')
        self.work = base / 'work'
        for dist in ('uniform', 'zipfian'):
            directory = self.work / dist; directory.mkdir(parents=True)
            (directory / 'warm.txt').write_text('I user1 value\n' * 2000)
            (directory / 'ycsba.txt').write_text('R user1\nU user1 value\n' * 500)
        binary = base / 'bin'; binary.mkdir()
        cmake = binary / 'cmake'; cmake.write_text(CMAKE); cmake.chmod(0o755)
        template = base / 'database.py'; template.write_text(DATABASE)
        spdk = base / 'spdk'; spdk.mkdir()
        self.result = base / 'results'
        self.audit = base / 'audit.jsonl'
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(('LIBDB_', 'UNIFORM_', 'ZIPFIAN_', 'MOCK_'))}
        self.env.update(PATH=str(binary) + os.pathsep + os.environ['PATH'],
            SPDK_ROOT=str(spdk), PMEM_BASE=str(self.pmem), RESULT_BASE=str(self.result),
            WORKLOAD_ROOT=str(self.work), PCI_ADDR='0000:00:01.0', USE_SUDO='0',
            SKIP_DRIVER_CHECK='1', LIBDB_ALLOW_DESTRUCTIVE='1', AUDIT=str(self.audit),
            MOCK_TEMPLATE=str(template), TIMEOUT_SECONDS='0', KILL_AFTER_SECONDS='1',
            THREADS='16', LOAD_INJECTORS='8', KV_COUNT='100000000', BOUNDARY_SIZE='1000',
            BUILD_JOBS='6')
        self.env.pop('REPEATS', None)

    def run_script(self, **env):
        return subprocess.run([str(SCRIPT)], env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=30)

    def events(self):
        return [json.loads(line) for line in self.audit.read_text().splitlines()] if self.audit.exists() else []

    def folder(self):
        folders = list(self.result.iterdir()); self.assertEqual(len(folders), 1)
        return folders[0]

    def rows(self, name='summary.csv'):
        with (self.folder() / name).open() as stream:
            return list(csv.DictReader(stream))

    def clean(self):
        self.assertEqual(list(self.pmem.iterdir()), [self.pmem / 'keep'])
        self.assertEqual((self.pmem / 'keep').read_text(), 'unrelated')

    def test_a_only_matrix_without_e_inputs(self):
        self.assertFalse(list(self.work.glob('*/ycsbe.txt')))
        result = self.run_script(UNIFORM_YCSBE_FILE='/nonexistent/uniform/E',
                                 ZIPFIAN_YCSBE_FILE='/nonexistent/zipfian/E', KV_SIZE='999', CACHE_SIZE_MB='1', TASKS_PER_THREAD='2',
                                 BINARY='/nonexistent', LIBDB_DYNAMIC_FLUSH_MODES='off')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        events = self.events()
        self.assertEqual([e['event'] for e in events[:8]], ['configure', 'build'] * 4)
        for event in events[:8:2]:
            self.assertEqual(event['args'][event['args'].index('-S') + 1], str(REPO))
        config = json.loads((self.folder() / 'config.json').read_text())
        self.assertEqual(config['SOURCE_DIR'], str(REPO))
        self.assertEqual(config['FLUSH_THRESHOLDS'], {
            str(kv): dict(flush_block_count=blocks, flush_threshold_bytes=blocks * 4096)
            for kv, blocks in ((1024, 4), (512, 2), (256, 1), (128, 1))})
        runs = events[8:]
        self.assertEqual(len(runs), 8)
        expected = [(p, dist, kind) for p in PROFILES for dist, kind in (('uniform', 'A'), ('zipfian', 'A'))]
        for event, (profile, dist, kind), row in zip(runs, expected, self.rows()):
            kv, cache, hard, high, low, flush_blocks = profile
            args, flags = event['args'], event['flags']
            self.assertEqual(event['event'], 'database')
            for key, value in (('kvsize', kv), ('cachesize', cache), ('tasks-per-thread', 16)):
                self.assertEqual(args[key], str(value))
            self.assertEqual(args['test'], str(self.work / dist / f'ycsb{kind.lower()}.txt'))
            self.assertEqual(args['warm'], str(self.work / dist / 'warm.txt'))
            for key, value in (('LIBDB_WBF_CAPACITY_LIMIT_BYTES', hard * GIB),
                               ('LIBDB_NVM_FLUSH_HIGH_BYTES', high * GIB),
                               ('LIBDB_NVM_FLUSH_LOW_BYTES', low * GIB)):
                self.assertEqual(flags[key], f'{value}ULL')
            self.assertEqual(flush_blocks, {1024: 4, 512: 2, 256: 1, 128: 1}[kv])
            self.assertEqual(flags['LIBDB_FLUSH_BLOCK_COUNT'], str(flush_blocks))
            self.assertEqual(row['flush_block_count'], str(flush_blocks))
            self.assertEqual(row['flush_threshold_bytes'], str(flush_blocks * 4096))
            self.assertEqual(flags['LIBDB_DYNAMIC_FLUSH_ENABLED'], '1')
            self.assertEqual(flags['LIBDB_NVM_FLUSH_POLL_NS'], '100000000ULL')
            self.assertNotIn('precondition', args)
            self.assertEqual(row['workload'], 'A')
            self.assertEqual(row['precondition_file'], '')
            self.assertEqual(row['precondition_requests'], '0')
            self.assertEqual(float(row['throughput_ops_s']), 100)
            self.assertEqual(row['nvm_sampled_peak_bytes'], '22')
            self.assertEqual(row['status'], 'ok')
            self.assertEqual(row['wbf_file_peak_scope'], 'whole_run')
        self.assertEqual(len({e['args']['pmem-dir'] for e in runs}), 8)
        groups = self.rows('aggregate.csv')
        self.assertEqual(len(groups), 8)
        for row in groups:
            blocks = {1024: 4, 512: 2, 256: 1, 128: 1}[int(row['kv_size_bytes'])]
            self.assertEqual(int(row['flush_block_count']), blocks)
            self.assertEqual(int(row['flush_threshold_bytes']), blocks * 4096)
        self.assertTrue(all(r['workload'] == 'A' for r in groups))
        self.assertTrue(all(r['successful_runs'] == '1' and float(r['stddev_ops_s']) == 0 for r in groups))
        self.assertEqual(json.loads((self.folder() / 'status.json').read_text())['status'], 'ok')
        self.clean()

    def test_repeats(self):
        result = self.run_script(REPEATS='2')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.rows()), 16)
        self.assertEqual([r['round'] for r in self.rows()], ['1'] * 8 + ['2'] * 8)
        self.assertTrue(all(r['successful_runs'] == '2' for r in self.rows('aggregate.csv')))
        self.clean()

    def test_input_and_repeat_errors_before_build(self):
        for value in ('0', '-1', 'abc'):
            self.assertNotEqual(self.run_script(REPEATS=value).returncode, 0)
        file = self.work / 'zipfian/ycsba.txt'
        file.write_text(file.read_text() + 'R user1\n')
        self.assertNotEqual(self.run_script().returncode, 0)
        file.unlink()
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertFalse(self.events())
        self.clean()

    def test_build_failure_before_any_database(self):
        result = self.run_script(MOCK_BUILD_FAIL='256')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(e['event'] == 'database' for e in self.events()))
        self.assertEqual(self.rows(), [])
        self.clean()

    def test_a_failure_stops_remaining_runs(self):
        result = self.run_script(MOCK_MODE='fail')
        self.assertNotEqual(result.returncode, 0)
        rows = self.rows()
        self.assertEqual([r['status'] for r in rows], ['failed'])
        self.assertEqual(rows[-1]['throughput_ops_s'], '')
        log = next(self.folder().glob('*uniform_A.log')).read_text()
        self.assertNotIn('begin precondition', log)
        self.assertEqual(len([e for e in self.events() if e['event'] == 'database']), 1)
        self.assertEqual(self.rows('aggregate.csv')[-1]['successful_runs'], '0')
        self.clean()

    def test_reject_wrong_threshold_and_missing_or_duplicate_statistics(self):
        for mode in ('wrong_threshold', 'missing_stats', 'duplicate_rate', 'wrong_ops', 'missing_end',
                     'wrong_flush', 'missing_flush', 'duplicate_flush'):
            with self.subTest(mode=mode):
                result_base = self.base / mode
                result = self.run_script(MOCK_MODE=mode, RESULT_BASE=str(result_base))
                self.assertNotEqual(result.returncode, 0, result.stdout)
                folder = next(result_base.iterdir())
                with (folder / 'summary.csv').open() as stream: rows = list(csv.DictReader(stream))
                self.assertEqual(rows[-1]['status'], 'invalid_result')
                self.assertEqual(rows[-1]['throughput_ops_s'], '')
                self.clean()

    def test_hard_capacity_failure(self):
        result = self.run_script(MOCK_MODE='cap_fail')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[0]['status'], 'capacity_limit')
        self.assertEqual(self.rows()[0]['wbf_capacity_limit_hit'], '1')
        self.clean()

    def test_timeout(self):
        result = self.run_script(MOCK_MODE='sleep', TIMEOUT_SECONDS='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.rows()[-1]['status'], 'timeout')
        self.clean()

    def test_interrupt_stops_database_before_cleanup(self):
        with tempfile.TemporaryFile() as output:
            process = subprocess.Popen([str(SCRIPT)], env=dict(self.env, MOCK_MODE='sleep'),
                                       stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                child = None
                while time.monotonic() < deadline:
                    runs = [e for e in self.events() if e['event'] == 'database']
                    if len(runs) == 1:
                        child = runs[-1]['pid']; break
                    if process.poll() is not None: break
                    time.sleep(.02)
                self.assertIsNotNone(child)
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=5), 143)
                with self.assertRaises(ProcessLookupError): os.kill(child, 0)
                self.assertEqual(self.rows()[-1]['status'], 'interrupted')
                self.clean()
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()

    def test_retained_e_parser(self):
        binary = self.base / 'mock_e'
        binary.write_text(DATABASE); binary.chmod(0o755)
        flags = dict(LIBDB_DYNAMIC_FLUSH_ENABLED='1', LIBDB_FLUSH_BLOCK_COUNT='4',
                     LIBDB_NVM_FLUSH_HIGH_BYTES=str(14 * GIB),
                     LIBDB_NVM_FLUSH_LOW_BYTES=str(12 * GIB),
                     LIBDB_WBF_CAPACITY_LIMIT_BYTES=str(16 * GIB))
        (self.base / 'flags.json').write_text(json.dumps(flags))
        for mode in ('', 'missing_pre_end'):
            with self.subTest(mode=mode):
                database = self.base / ('db_e_' + mode); database.mkdir()
                output = self.base / ('e_' + mode + '.result')
                log = self.base / ('e_' + mode + '.log')
                trace = self.work / 'zipfian/ycsbe.txt'
                pre = self.work / 'uniform/ycsba.txt'
                command = [str(binary), '--pmem-dir=' + str(database),
                           '--test=' + str(trace), '--precondition=' + str(pre),
                           '--output=' + str(output)]
                with log.open('w') as stream:
                    process = subprocess.run(command, env=dict(self.env, MOCK_MODE=mode),
                                             stdout=stream, stderr=subprocess.STDOUT, timeout=5)
                self.assertEqual(process.returncode, 0)
                kwargs = dict(workload='E', test_file=trace, expected=3000, warm_expected=2000,
                              profile=PROFILES[0], precondition_file=pre, precondition_expected=1000)
                if mode:
                    with self.assertRaises(ValueError): measured_result(log, output, **kwargs)
                else:
                    metrics = measured_result(log, output, **kwargs)
                    self.assertEqual(float(metrics['throughput_ops_s']), 200)
                    self.assertEqual(metrics['nvm_sampled_peak_bytes'], 22)

    def test_flush_macro_compilation(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        for override, expected in ((None, 4), ('1', 1), ('2', 2), ('4', 4),
                                   ('0', None), ('-1', None), ('1.5', None)):
            with self.subTest(override=override):
                source = '#include "config.h"\n'
                if expected is not None:
                    source += f'_Static_assert(LIBDB_FLUSH_BLOCK_COUNT == {expected}, "override lost");\n'
                    source += f'_Static_assert(BLOCK_SIZE * Flush_Blcok_Num == {4096 * expected}, "wrong bytes");\n'
                command = [compiler, '-std=c11', '-Werror', '-fsyntax-only', '-x', 'c', '-I', str(REPO)]
                if override is not None: command.append('-DLIBDB_FLUSH_BLOCK_COUNT=' + override)
                result = subprocess.run(command + ['-'], input=source, capture_output=True, text=True)
                if expected is None: self.assertNotEqual(result.returncode, 0)
                else: self.assertEqual(result.returncode, 0, result.stderr)

    def test_aggregate_separates_flush_thresholds(self):
        summary = self.base / 'thresholds.csv'
        fields = ['kv_size_bytes', 'cache_mib', 'distribution', 'workload', 'tasks_per_thread',
                  'nvm_hard_limit_bytes', 'nvm_limit_bytes', 'nvm_low_bytes',
                  'flush_block_count', 'flush_threshold_bytes', 'status', 'throughput_ops_s']
        with summary.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader()
            for blocks, rate in ((4, 100), (2, 300)):
                writer.writerow(dict(dict.fromkeys(fields, ''), kv_size_bytes=512,
                    distribution='uniform', workload='A', status='ok', throughput_ops_s=rate,
                    flush_block_count=blocks, flush_threshold_bytes=blocks * 4096))
        aggregate(summary, self.base / 'thresholds-aggregate.csv')
        with (self.base / 'thresholds-aggregate.csv').open() as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual([row['flush_block_count'] for row in rows], ['4', '2'])
        self.assertEqual([float(row['mean_ops_s']) for row in rows], [100, 300])

    def test_aggregate_ignores_failure(self):
        summary = self.base / 'summary.csv'
        fields = ['kv_size_bytes', 'cache_mib', 'distribution', 'workload', 'tasks_per_thread',
                  'nvm_hard_limit_bytes', 'nvm_limit_bytes', 'nvm_low_bytes', 'status', 'throughput_ops_s']
        with summary.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader()
            for status, rate in [('ok', 100), ('failed', 9999), ('ok', 300)]:
                writer.writerow(dict(dict.fromkeys(fields, ''), kv_size_bytes=1024,
                    distribution='uniform', workload='E', status=status, throughput_ops_s=rate))
        aggregate(summary, self.base / 'aggregate.csv')
        with (self.base / 'aggregate.csv').open() as stream: row = next(csv.DictReader(stream))
        self.assertEqual(row['successful_runs'], '2')
        self.assertEqual(float(row['mean_ops_s']), 200)
        self.assertAlmostEqual(float(row['stddev_ops_s']), 200 ** .5 * 10)


if __name__ == '__main__': unittest.main()
