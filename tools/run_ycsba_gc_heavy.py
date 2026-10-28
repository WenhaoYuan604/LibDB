#!/usr/bin/env python3
"""Build current LibDB-clean and measure the final 100M uniform YCSB A requests."""
import argparse
import csv
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from run_ycsba_uniform_zipfian import Interrupted, interrupt, positive, existing, execute
from run_ycsba_dynamic_flush import save_json
from summarize_ycsba_gc_heavy import inspect_trace, summarize, BYTE_FIELDS

REPO = Path(__file__).resolve().parent.parent
COUNTS = {'load': 100_000_000, 'precondition': 150_000_000, 'measured': 100_000_000}
FIELDS = ['status', 'error', 'distribution', 'workload', 'workers', 'injectors',
          'cache_mib', 'tasks_per_thread', 'phase', 'requests', 'inserts', 'updates',
          'reads', 'scans', 'kv_size_bytes', 'seconds', *BYTE_FIELDS,
          'logical_update_bytes', 'ssd_write_bytes', 'throughput_ops_s',
          'throughput_mops_s', 'ssd_write_amplification', 'gc_observed']


def help_parser():
    return argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''Run: ./run_ycsba_gc_heavy.sh
Fixed trace sizes: load 100M -> cache warmup -> precondition 150M -> measured 100M.
Environment defaults:
  WORKLOAD_ROOT=/home/why/LibDB/workloads_unif
  WARM_FILE=$WORKLOAD_ROOT/warm.txt
  PRECONDITION_FILE=$WORKLOAD_ROOT/ycsba1.txt
  TEST_FILE=$WORKLOAD_ROOT/ycsba2.txt
  THREADS=16 LOAD_INJECTORS=8 KV_SIZE=1024 CACHE_SIZE_MB=20480 (20 GiB)
  TASKS_PER_THREAD=16 BOUNDARY_SIZE=1000
  PCI_ADDR=0000:5c:00.0 PMEM_BASE=/mnt/whypmem LIBDB_ALLOW_DESTRUCTIVE=1
  SPDK_ROOT=<repo>/../spdk BUILD_JOBS=6 RESULT_BASE=<repo>/results USE_SUDO=1
  TIMEOUT_SECONDS=0 KILL_AFTER_SECONDS=30
  SKIP_DRIVER_CHECK=0 (1 only for mock tests)
One fresh database, one run; BINARY and REPEATS are ignored. Build current source
in the result directory, retaining current GC/Flush configuration. No driver or
host tuning changes. Only this run's private PMEM child is removed.
SSD WA = successful SSD write completion bytes during the measured window /
(actual UPDATE count * KV_SIZE). Includes Flush/GC/Split/Merge, excludes PMEM;
not NAND amplification. No forced drain at window boundaries. Atomic counters
are sampled sequentially; background completions are attributed by window time.
Input loading and report printing are outside the window. No-GC results are
explicitly marked gc_observed=false, not certified as GC-heavy.
''')


def configuration():
    c = {name: positive(name, default, minimum, maximum) for name, default, minimum, maximum in (
        ('THREADS', 16, 1, 126), ('LOAD_INJECTORS', 8, 1, 126),
        ('KV_SIZE', 1024, 52, 32768), ('CACHE_SIZE_MB', 20480, 0, 2147483647),
        ('TASKS_PER_THREAD', 16, 1, 2147483647), ('BOUNDARY_SIZE', 1000, 1, 2147483647),
        ('BUILD_JOBS', 6, 1, 1024), ('USE_SUDO', 1, 0, 1),
        ('SKIP_DRIVER_CHECK', 0, 0, 1), ('TIMEOUT_SECONDS', 0, 0, 2147483647),
        ('KILL_AFTER_SECONDS', 30, 1, 2147483647))}
    if c['THREADS'] + c['LOAD_INJECTORS'] + 1 > 128:
        raise ValueError('threads + injectors + 1 must not exceed 128')
    c['LIBDB_ALLOW_DESTRUCTIVE'] = os.environ.get('LIBDB_ALLOW_DESTRUCTIVE', '1')
    if c['LIBDB_ALLOW_DESTRUCTIVE'] != '1':
        raise ValueError('LIBDB_ALLOW_DESTRUCTIVE must be 1 for the disposable experiment SSD')
    c['PCI_ADDR'] = os.environ.get('PCI_ADDR', '0000:5c:00.0')
    if not re.fullmatch(r'[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[01][0-9a-fA-F]\.[0-7]', c['PCI_ADDR']):
        raise ValueError('PCI_ADDR must be DDDD:BB:DD.F')
    c['PMEM_BASE'] = existing('PMEM_BASE', '/mnt/whypmem', directory=True)
    if c['PMEM_BASE'] == Path('/'):
        raise ValueError('PMEM_BASE must not be /')
    c['SPDK_ROOT'] = existing('SPDK_ROOT', REPO.parent / 'spdk', directory=True)
    c['RESULT_BASE'] = Path(os.environ.get('RESULT_BASE', REPO / 'results')).resolve()
    for tool in ('cmake', 'stdbuf'):
        c[tool.upper()] = shutil.which(tool)
        if not c[tool.upper()]: raise ValueError(f'{tool} is required')
    if not c['SKIP_DRIVER_CHECK']:
        driver = Path('/sys/bus/pci/devices') / c['PCI_ADDR'] / 'driver'
        if not driver.is_symlink() or driver.resolve().name not in ('uio_pci_generic', 'vfio-pci'):
            raise ValueError('experiment SSD must already be bound to uio_pci_generic or vfio-pci')
        c['PCI_DRIVER'] = driver.resolve().name
    else:
        c['PCI_DRIVER'] = 'check_skipped'
    root = Path(os.environ.get('WORKLOAD_ROOT', '/home/why/LibDB/workloads_unif'))
    c['INPUTS'] = {}
    for phase, key, filename in (('load', 'WARM_FILE', 'warm.txt'),
            ('precondition', 'PRECONDITION_FILE', 'ycsba1.txt'), ('measured', 'TEST_FILE', 'ycsba2.txt')):
        c[key] = existing(key, root / filename)
        if 'Production' in str(c[key]): raise ValueError('Production trace paths are not supported')
        print(f'Validating {phase}: {c[key]} ({COUNTS[phase]} requests)', flush=True)
        c['INPUTS'][phase] = inspect_trace(c[key], phase, COUNTS[phase])
    c['ROOT_COMMAND'] = []
    if c['USE_SUDO'] and os.geteuid() != 0:
        subprocess.run(['sudo', '-v'], check=True)
        c['ROOT_COMMAND'] = ['sudo', '-n']
    for permission in ('-w', '-x'):
        subprocess.run(c['ROOT_COMMAND'] + ['test', permission, str(c['PMEM_BASE'])], check=True)
    return c


def run(c):
    c['RESULT_BASE'].mkdir(parents=True, exist_ok=True)
    result = Path(tempfile.mkdtemp(prefix=time.strftime('ycsba_gc_heavy_%Y%m%d_%H%M%S_'), dir=c['RESULT_BASE']))
    print(f'Results: {result}', flush=True)
    save_json(result / 'config.json', c)
    status = dict(status='running', stage='build', error='')
    row = dict(status='failed', error='', distribution='uniform', workload='A',
               workers=c['THREADS'], injectors=c['LOAD_INJECTORS'],
               cache_mib=c['CACHE_SIZE_MB'], tasks_per_thread=c['TASKS_PER_THREAD'])
    pmem = None
    try:
        build = result / 'build'
        for step, command in (
            ('configure', [c['CMAKE'], '-S', str(REPO), '-B', str(build),
                           '-DBUILD_TESTING=OFF', f'-DSPDK_ROOT={c["SPDK_ROOT"]}']),
            ('build', [c['CMAKE'], '--build', str(build), '--target', 'libdb', '--parallel', str(c['BUILD_JOBS'])])):
            status['stage'] = step
            save_json(result / 'status.json', status)
            save_json(result / f'{step}.command.json', command)
            execute(command, result / f'{step}.log', dict(c, ROOT_COMMAND=[]))
        binary = build / 'test'
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError('build did not produce executable test')
        status['stage'] = 'database'
        pmem = Path(subprocess.check_output(c['ROOT_COMMAND'] + ['mktemp', '-d',
            str(c['PMEM_BASE'] / 'libdb_gc_heavy_XXXXXXXXXX')], text=True).strip())
        c['PMEM_DIR'] = pmem
        save_json(result / 'config.json', c)
        save_json(result / 'status.json', status)
        args = {'thread': c['THREADS'], 'load-injectors': c['LOAD_INJECTORS'],
                'kvsize': c['KV_SIZE'], 'kvs': c['INPUTS']['load']['requests'],
                'cachesize': c['CACHE_SIZE_MB'], 'tasks-per-thread': c['TASKS_PER_THREAD'],
                'boundary-size': c['BOUNDARY_SIZE'], 'pci-addr': c['PCI_ADDR'],
                'pmem-dir': pmem, 'warm': c['WARM_FILE'], 'precondition': c['PRECONDITION_FILE'],
                'test': c['TEST_FILE'], 'output': result / 'uniform_A.result.txt'}
        command = c['ROOT_COMMAND'] + [c['STDBUF'], '-oL', '-eL', str(binary)]
        command += [f'--{key}={value}' for key, value in args.items()]
        save_json(result / 'uniform_A.command.json', command)
        execute(command, result / 'uniform_A.log', c)
        status['stage'] = 'summarize'
        metrics = summarize(result / 'uniform_A.log', args['output'], c['INPUTS'], c['KV_SIZE'])
        row.update(metrics, status='ok')
        status.update(status='ok', stage='complete')
    except Exception as error:
        state = 'interrupted' if isinstance(error, Interrupted) else 'timeout' if isinstance(error, TimeoutError) else 'failed'
        row.update(status=state, error=str(error))
        status.update(status=state, error=str(error))
        raise
    finally:
        try:
            if pmem is not None and not c.get('_PROCESS_UNSTOPPED'):
                subprocess.run(c['ROOT_COMMAND'] + ['rm', '-rf', '--', str(pmem)], check=True)
            elif c.get('_PROCESS_UNSTOPPED'):
                print(f'Process still running; private PMEM preserved: {pmem}', file=sys.stderr)
        except Exception as error:
            status.update(status='cleanup_failed', error=str(error))
            row.update(status='cleanup_failed', error=str(error))
            raise
        finally:
            with (result / 'summary.csv').open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerow(row)
            save_json(result / 'status.json', status)
            print(f'Results and logs retained: {result}', flush=True)
    print(f'Final {row["requests"]:,} ops: {row["throughput_ops_s"]:.3f} ops/s '
          f'({row["throughput_mops_s"]:.6f} Mops/s), SSD WA={row["ssd_write_amplification"]:.6f}x', flush=True)
    print('GC writes observed in measured window.' if row['gc_observed'] else
          'No GC writes observed in measured window; GC-heavy is NOT verified.', flush=True)
    return result


def main():
    help_parser().parse_args()
    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    try:
        run(configuration())
    except Interrupted as error:
        print(f'error: {error}', file=sys.stderr)
        return 128 + error.number
    except Exception as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
