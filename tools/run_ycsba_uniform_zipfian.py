#!/usr/bin/env python3
"""Run uniform/zipfian YCSB A and F in three rounds; never tune the host."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from summarize_ycsba import FIELDS, aggregate, inspect_input, measured_result

REPO = Path(__file__).resolve().parent.parent


class Interrupted(Exception):
    def __init__(self, number):
        self.number = number
        super().__init__(f'interrupted by signal {number}')


def interrupt(number, frame):
    raise Interrupted(number)


def positive(name, default, minimum=1, maximum=2147483647):
    raw = os.environ.get(name, str(default))
    if not re.fullmatch(r'[0-9]+', raw) or not minimum <= int(raw) <= maximum:
        raise ValueError(f'{name} must be an integer in [{minimum}, {maximum}]')
    return int(raw)


def existing(name, default, directory=False):
    path = Path(os.environ.get(name, str(default))).resolve(strict=True)
    if not (path.is_dir() if directory else path.is_file()):
        raise ValueError(f'{name}: wrong path type: {path}')
    if not os.access(path, os.R_OK):
        raise ValueError(f'{name}: unreadable: {path}')
    return path


def help_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''Run directly: sudo ./run_ycsba_uniform_zipfian.sh
Defaults (all may be overridden by environment):
  PCI_ADDR=0000:5c:00.0   Dedicated, already SPDK-bound experiment SSD
  PMEM_BASE=/mnt/whypmem   Existing PMEM directory
  LIBDB_ALLOW_DESTRUCTIVE=1
  WORKLOAD_ROOT=/mnt/nvme0n1/why/ycsb_workloads_one_warm
  UNIFORM_WARM_FILE=$WORKLOAD_ROOT/uniform/warm.txt
  UNIFORM_YCSBA_FILE=$WORKLOAD_ROOT/uniform/ycsba.txt
  UNIFORM_YCSBF_FILE=$WORKLOAD_ROOT/uniform/ycsbf.txt
  ZIPFIAN_WARM_FILE=$WORKLOAD_ROOT/zipfian/warm.txt
  ZIPFIAN_YCSBA_FILE=$WORKLOAD_ROOT/zipfian/ycsba.txt
  ZIPFIAN_YCSBF_FILE=$WORKLOAD_ROOT/zipfian/ycsbf.txt
  THREADS=16 LOAD_INJECTORS=8 KV_SIZE=1024 KV_COUNT=100000000
  CACHE_SIZE_MB=20000 TASKS_PER_THREAD=16 BOUNDARY_SIZE=1000
  BINARY=<repo>/build/test RESULT_BASE=<repo>/results USE_SUDO=1
  TIMEOUT_SECONDS=0 KILL_AFTER_SECONDS=30
  SKIP_DRIVER_CHECK=0 (1 is intended only for mock-program tests)
No build, workload generation, CPU/NUMA tuning, formatting, mounting or driver changes.
Only the newly created private PMEM child directory is removed on exit.
Runs three rounds of uniform A, uniform F, zipfian A, zipfian F (12 fresh databases).
REPEATS is ignored. F counts executed READ/UPDATE requests, not logical RMW transactions.
F is truncated to whole thousands by the database; raw/executed/tail counts are recorded.
Logs, config.json, summary.csv and aggregate.csv remain in the result directory.''')
    return parser


def configuration():
    c = {name: positive(name, default, minimum, maximum) for name, default, minimum, maximum in (
        ('THREADS', 16, 1, 126),
        ('LOAD_INJECTORS', 8, 1, 126), ('KV_SIZE', 1024, 52, 32768),
        ('KV_COUNT', 100000000, 1, 2147483647), ('CACHE_SIZE_MB', 20000, 0, 2147483647),
        ('TASKS_PER_THREAD', 16, 1, 2147483647), ('BOUNDARY_SIZE', 1000, 1, 2147483647),
        ('USE_SUDO', 1, 0, 1), ('SKIP_DRIVER_CHECK', 0, 0, 1),
        ('TIMEOUT_SECONDS', 0, 0, 2147483647), ('KILL_AFTER_SECONDS', 30, 1, 2147483647))}
    c['REPEATS'] = 3
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
    for dist in ('uniform', 'zipfian'):
        for kind, filename in (('WARM', 'warm.txt'), ('YCSBA', 'ycsba.txt'), ('YCSBF', 'ycsbf.txt')):
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
            cache_key = (path, key.endswith('_YCSBF_FILE'))
            if cache_key not in cache:
                cache[cache_key] = inspect_input(path, is_f=cache_key[1])
            c[key + '_INFO'] = cache[cache_key]
            c[key + '_REQUESTS'] = cache[cache_key]['requests']
    return c


def stop_process(proc, root, grace):
    if proc.poll() is not None:
        return
    def send(sig):
        if root:
            # Signal the privileged database too, not just sudo's user-owned monitor.
            subprocess.run(root + ['kill', f'-{int(sig)}', '--', f'-{proc.pid}'], check=True)
        else:
            try:
                os.killpg(proc.pid, sig)
            except ProcessLookupError:
                pass
    send(signal.SIGINT)
    try:
        proc.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        send(signal.SIGKILL)
        proc.wait()


def execute(command, log, c):
    # A private process group allows timeout/interruption to stop the database
    # before the caller removes PMEM. Merge stderr into stdout, retaining GC logs.
    with log.open('xb') as output:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                start_new_session=True)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(proc.stdout, selectors.EVENT_READ)
                start = time.monotonic()
                while selector.get_map():
                    if c['TIMEOUT_SECONDS'] and time.monotonic() - start >= c['TIMEOUT_SECONDS']:
                        raise TimeoutError('phase timeout')
                    for key, _ in selector.select(0.2):
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        output.write(chunk)
                        output.flush()
                        sys.stdout.buffer.write(chunk)
                        sys.stdout.buffer.flush()
                remaining = (max(0.001, c['TIMEOUT_SECONDS'] - (time.monotonic() - start))
                             if c['TIMEOUT_SECONDS'] else None)
                try:
                    code = proc.wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    raise TimeoutError('phase timeout')
                if code:
                    raise RuntimeError(f'program exit={code}')
        finally:
            # Ignore repeated terminal signals while stopping the child.
            old = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
            try:
                stop_process(proc, c['ROOT_COMMAND'], c['KILL_AFTER_SECONDS'])
            except Exception:
                c['_PROCESS_UNSTOPPED'] = proc.poll() is None
                raise
            finally:
                proc.stdout.close()
                for sig, handler in old.items():
                    signal.signal(sig, handler)


def run(c):
    c['RESULT_BASE'].mkdir(parents=True, exist_ok=True)
    result = Path(tempfile.mkdtemp(prefix=time.strftime('ycsba_%Y%m%d_%H%M%S_'), dir=c['RESULT_BASE']))
    summary = result / 'summary.csv'
    with summary.open('x', newline='') as stream:
        csv.DictWriter(stream, fieldnames=FIELDS).writeheader()
    pmem = None
    print(f'Results: {result}', flush=True)
    try:
        # mktemp creates a private, unique child, never a user-supplied deletion path.
        pmem = Path(subprocess.check_output(c['ROOT_COMMAND'] + ['mktemp', '-d',
            str(c['PMEM_BASE'] / 'libdb_ycsba_XXXXXXXXXX')], text=True).strip())
        c['PMEM_DIR'] = pmem
        (result / 'config.json').write_text(json.dumps(c, default=str, indent=2) + '\n')
        for repeat in range(1, c['REPEATS'] + 1):
            for dist, kind in (('uniform', 'A'), ('uniform', 'F'), ('zipfian', 'A'), ('zipfian', 'F')):
                label = f'{repeat:02d}_{dist}_{kind}'
                warm = c[f'{dist.upper()}_WARM_FILE']
                workload = c[f'{dist.upper()}_YCSB{kind}_FILE']
                info = c[f'{dist.upper()}_YCSB{kind}_FILE_INFO']
                expected = info['requests']
                row = dict(distribution=dist, workload=kind, round=repeat, workers=c['THREADS'],
                    injectors=c['LOAD_INJECTORS'], kv_size_bytes=c['KV_SIZE'],
                    cache_mib=c['CACHE_SIZE_MB'], requests=expected,
                    raw_requests=info['raw_requests'], tail_requests=info['tail_requests'],
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
                    seconds, rate = measured_result(result / f'{label}.log', args['output'],
                        str(workload), expected, c[f'{dist.upper()}_WARM_FILE_REQUESTS'],
                        tail_operations=info['tail_operations'])
                    row.update(seconds=seconds, throughput_ops_s=rate, status='ok')
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
