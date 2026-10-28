#!/usr/bin/env python3
"""Run uniform YCSB E latency three times with two tasks per worker."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from summarize_ycsbe_latency import FIELDS, aggregate, measured_result
from summarize_ycsba import count_requests
from run_ycsba_uniform_zipfian import Interrupted, interrupt, positive, existing, execute

REPO = Path(__file__).resolve().parent.parent


def help_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''Run directly: sudo ./run_ycsbe_latency_task2.sh
Defaults (all may be overridden by environment):
  PCI_ADDR=0000:5c:00.0   Dedicated, already SPDK-bound experiment SSD
  PMEM_BASE=/mnt/whypmem   Existing PMEM directory
  LIBDB_ALLOW_DESTRUCTIVE=1
  WORKLOAD_ROOT=/mnt/nvme0n1/why/ycsb_workloads_one_warm
  UNIFORM_WARM_FILE=$WORKLOAD_ROOT/uniform/warm.txt
  UNIFORM_YCSBE_FILE=$WORKLOAD_ROOT/uniform/ycsbe.txt
  THREADS=16 LOAD_INJECTORS=8 KV_SIZE=1024 KV_COUNT=100000000
  CACHE_SIZE_MB=20000 BOUNDARY_SIZE=1000
  TASKS_PER_THREAD is fixed at 2; REPEATS is fixed at 3 (environment ignored).
  BINARY=<repo>/build/test RESULT_BASE=<repo>/results USE_SUDO=1
  TIMEOUT_SECONDS=0 KILL_AFTER_SECONDS=30
  SKIP_DRIVER_CHECK=0 (1 is intended only for mock-program tests)
No build, workload generation, CPU/NUMA tuning, formatting, mounting or driver changes.
Only the newly created private PMEM child directory is removed on exit.
Runs three fresh databases: load, cache warmup, then uniform E (no A preconditioning).
Latency includes all E requests, in microseconds, from worker dispatch to result creation.
Mean is a histogram midpoint estimate; percentiles/max use bucket upper bounds.
Cross-run percentile averages are not pooled percentiles.
Logs, config.json, summary.csv and aggregate.csv remain in the result directory.''')
    return parser


def configuration():
    c = {name: positive(name, default, minimum, maximum) for name, default, minimum, maximum in (
        ('THREADS', 16, 1, 126),
        ('LOAD_INJECTORS', 8, 1, 126), ('KV_SIZE', 1024, 52, 32768),
        ('KV_COUNT', 100000000, 1, 2147483647), ('CACHE_SIZE_MB', 20000, 0, 2147483647),
        ('BOUNDARY_SIZE', 1000, 1, 2147483647),
        ('USE_SUDO', 1, 0, 1), ('SKIP_DRIVER_CHECK', 0, 0, 1),
        ('TIMEOUT_SECONDS', 0, 0, 2147483647), ('KILL_AFTER_SECONDS', 30, 1, 2147483647))}
    c['REPEATS'] = 3
    c['TASKS_PER_THREAD'] = 2
    if c['THREADS'] + c['LOAD_INJECTORS'] + 1 > 128:
        raise ValueError('threads + injectors + 1 must not exceed 128')
    c['LIBDB_ALLOW_DESTRUCTIVE'] = os.environ.get('LIBDB_ALLOW_DESTRUCTIVE', '1')
    if c['LIBDB_ALLOW_DESTRUCTIVE'] != '1':
        raise ValueError('set LIBDB_ALLOW_DESTRUCTIVE=1 for the disposable experiment SSD')
    pci = os.environ.get('PCI_ADDR', '0000:5c:00.0')
    if not re.fullmatch(r'[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[01][0-9a-fA-F]\.[0-7]', pci):
        raise ValueError('PCI_ADDR must identify the dedicated SSD as DDDD:BB:DD.F')
    c['PCI_ADDR'] = pci
    c['BINARY'] = existing('BINARY', REPO / 'build/test')
    if not os.access(c['BINARY'], os.X_OK):
        raise ValueError('BINARY is not executable')
    if os.environ.get('PMEM_BASE') == '':
        raise ValueError('PMEM_BASE must not be empty')
    c['PMEM_BASE'] = existing('PMEM_BASE', '/mnt/whypmem', directory=True)
    if c['PMEM_BASE'] == Path('/'):
        raise ValueError('PMEM_BASE must not be /')
    c['RESULT_BASE'] = Path(os.environ.get('RESULT_BASE', REPO / 'results')).resolve()
    root = Path(os.environ.get('WORKLOAD_ROOT', '/mnt/nvme0n1/why/ycsb_workloads_one_warm'))
    for dist in ('uniform',):
        for kind, filename in (('WARM', 'warm.txt'), ('YCSBE', 'ycsbe.txt')):
            key = f'{dist.upper()}_{kind}_FILE'
            c[key] = existing(key, root / dist / filename)
            if 'Production' in str(c[key]):
                raise ValueError(f'{key}: Production paths select a different database reader')
    if not c['SKIP_DRIVER_CHECK']:
        driver = Path('/sys/bus/pci/devices') / pci / 'driver'
        if not driver.is_symlink() or driver.resolve().name not in ('uio_pci_generic', 'vfio-pci'):
            raise ValueError(f'{pci}: expected an already bound uio_pci_generic or vfio-pci driver')
        c['PCI_DRIVER'] = driver.resolve().name
    else:
        c['PCI_DRIVER'] = 'check_skipped'
    c['ROOT_COMMAND'] = []
    if c['USE_SUDO'] and os.geteuid() != 0:
        if not shutil.which('sudo'):
            raise ValueError('sudo is required')
        subprocess.run(['sudo', '-v'], check=True)
        c['ROOT_COMMAND'] = ['sudo', '-n']
    subprocess.run(c['ROOT_COMMAND'] + ['test', '-w', str(c['PMEM_BASE'])], check=True)
    subprocess.run(c['ROOT_COMMAND'] + ['test', '-x', str(c['PMEM_BASE'])], check=True)
    # Count all inputs before creating or removing experiment storage.
    cache = {}
    for key, path in list(c.items()):
        if key.endswith('_FILE'):
            if path not in cache:
                cache[path] = count_requests(path)
            c[key + '_REQUESTS'] = cache[path]
    return c


def run(c):
    c['RESULT_BASE'].mkdir(parents=True, exist_ok=True)
    result = Path(tempfile.mkdtemp(prefix=time.strftime('ycsbe_latency_task2_%Y%m%d_%H%M%S_'), dir=c['RESULT_BASE']))
    summary = result / 'summary.csv'
    with summary.open('x', newline='') as stream:
        csv.DictWriter(stream, fieldnames=FIELDS).writeheader()
    pmem = None
    print(f'Results: {result}', flush=True)
    try:
        # mktemp creates a private, unique child, never a user-supplied deletion path.
        pmem = Path(subprocess.check_output(c['ROOT_COMMAND'] + ['mktemp', '-d',
            str(c['PMEM_BASE'] / 'libdb_ycsbe_latency_XXXXXXXXXX')], text=True).strip())
        c['PMEM_DIR'] = pmem
        (result / 'config.json').write_text(json.dumps(c, default=str, indent=2) + '\n')
        for repeat in range(1, c['REPEATS'] + 1):
            for dist, kind in (('uniform', 'E'),):
                label = f'{repeat:02d}_{dist}_{kind}'
                warm = c[f'{dist.upper()}_WARM_FILE']
                workload = c[f'{dist.upper()}_YCSB{kind}_FILE']
                expected = c['UNIFORM_YCSBE_FILE_REQUESTS']
                row = dict(distribution=dist, workload=kind, round=repeat, workers=c['THREADS'],
                    injectors=c['LOAD_INJECTORS'], kv_size_bytes=c['KV_SIZE'],
                    cache_mib=c['CACHE_SIZE_MB'], requests=expected,
                    tasks_per_thread=2,
                    seconds='', throughput_ops_s='', status='prepare_failed', error='')
                try:
                    # Preserve the private parent; only this phase's db child is reset.
                    database = pmem / 'db'
                    subprocess.run(c['ROOT_COMMAND'] + ['rm', '-rf', '--', str(database)], check=True)
                    subprocess.run(c['ROOT_COMMAND'] + ['mkdir', '--', str(database)], check=True)
                    args = {'thread': c['THREADS'], 'load-injectors': c['LOAD_INJECTORS'],
                        'kvsize': c['KV_SIZE'], 'kvs': c['KV_COUNT'], 'cachesize': c['CACHE_SIZE_MB'],
                        'tasks-per-thread': c['TASKS_PER_THREAD'], 'boundary-size': c['BOUNDARY_SIZE'],
                        'pci-addr': c['PCI_ADDR'], 'pmem-dir': database, 'warm': warm,
                        'test': workload, 'output': result / f'{label}.result.txt'}
                    command = c['ROOT_COMMAND'] + [str(c['BINARY'])] + [f'--{k}={v}' for k, v in args.items()]
                    (result / f'{label}.command.json').write_text(json.dumps(command, indent=2) + '\n')
                    row['status'] = 'failed'
                    print(f'Starting {label}', flush=True)
                    execute(command, result / f'{label}.log', c)
                    row['status'] = 'invalid_result'
                    metrics = measured_result(result / f'{label}.log', args['output'],
                        str(workload), expected, c[f'{dist.upper()}_WARM_FILE_REQUESTS'])
                    row.update(metrics, status='ok')
                except Exception as error:
                    if isinstance(error, Interrupted):
                        row['status'] = 'interrupted'
                    elif isinstance(error, TimeoutError):
                        row['status'] = 'timeout'
                    row['error'] = str(error)
                    raise
                finally:
                    with summary.open('a', newline='') as stream:
                        csv.DictWriter(stream, fieldnames=FIELDS).writerow(row)
                    aggregate(summary, result / 'aggregate.csv')
    finally:
        if pmem is not None and not c.get('_PROCESS_UNSTOPPED'):
            subprocess.run(c['ROOT_COMMAND'] + ['rm', '-rf', '--', str(pmem)], check=True)
        if c.get('_PROCESS_UNSTOPPED'):
            print(f'error: database could not be stopped; PMEM preserved at {pmem}', file=sys.stderr)
        print(f'Results and logs retained: {result}', flush=True)
    print((result / 'aggregate.csv').read_text(), end='')


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
