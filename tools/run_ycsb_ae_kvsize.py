#!/usr/bin/env python3
"""Build and run the uniform/zipfian YCSB A-only KV-size matrix."""
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
from run_ycsba_dynamic_flush import save_json, NVM_REJECTION
from summarize_ycsba import count_requests
from summarize_ycsb_ae_kvsize import PROFILES, CASES, GIB, BLOCK_SIZE, FIELDS, measured_result, aggregate

REPO = Path(__file__).resolve().parent.parent


def help_parser():
    return argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog='''
Run: sudo ./run_ycsb_ae_kvsize.sh
  REPEATS=1 (positive integer; each round runs 8 fresh databases)
  KV bytes / cache MiB / WBF hard,high,low GiB:
  1024/20480/16,14,12  512/10240/8,7,6  256/5120/4,3,2  128/1024/4,3,2
  Local Flush blocks for KV 1024/512/256/128: 4/2/1/1 (16/8/4/4 KiB).
  A foreground write may Flush only when buffered bytes strictly exceed this threshold
  and the current dynamic policy allows it.
  TASKS_PER_THREAD is fixed at 16; dynamic Flush ON, poll interval 100ms.
  A: load -> cache warmup -> measured A
  Only uniform A and zipfian A are run; no precondition phase or E input required.
  Persistence: existing PMEM header + 32-byte value prefix; no independent WAL.
Environment defaults:
  THREADS=16 LOAD_INJECTORS=8 KV_COUNT=100000000 BOUNDARY_SIZE=1000
  WORKLOAD_ROOT=/mnt/nvme0n1/why/ycsb_workloads_one_warm
  UNIFORM_WARM_FILE, UNIFORM_YCSBA_FILE default to
    $WORKLOAD_ROOT/uniform/{warm,ycsba}.txt; ZIPFIAN_* analogously.
  PCI_ADDR=0000:5c:00.0 PMEM_BASE=/mnt/whypmem LIBDB_ALLOW_DESTRUCTIVE=1
  SPDK_ROOT=<repo>/../spdk BUILD_JOBS=6 RESULT_BASE=<repo>/results USE_SUDO=1
  TIMEOUT_SECONDS=0 KILL_AFTER_SECONDS=30
  SKIP_DRIVER_CHECK=0 (1 only for mock tests)
KV_SIZE, CACHE_SIZE_MB, TASKS_PER_THREAD, BINARY and LIBDB_DYNAMIC_FLUSH_MODES
are ignored: the matrix and build options above are fixed. Four isolated builds
from the current working-tree source (including local changes) must succeed before testing. Timeout applies per configure/build/database process.
The database timeout covers load, warmup and measured A.
No workload generation, CPU/NUMA tuning or driver changes. Only this invocation's
private PMEM child is cleaned; the selected SSD must be disposable.
Logs stream live; config, commands, binaries, summary.csv and aggregate.csv remain.
Throughput excludes load and warmup. WBF file peak is for the whole run.
''')


def configuration():
    c = {name: positive(name, default, minimum, maximum) for name, default, minimum, maximum in (
        ('THREADS', 16, 1, 126), ('LOAD_INJECTORS', 8, 1, 126),
        ('KV_COUNT', 100000000, 1, 2147483647),
        ('REPEATS', 1, 1, 2147483647),
        ('BOUNDARY_SIZE', 1000, 1, 2147483647), ('BUILD_JOBS', 6, 1, 1024),
        ('USE_SUDO', 1, 0, 1), ('SKIP_DRIVER_CHECK', 0, 0, 1),
        ('TIMEOUT_SECONDS', 0, 0, 2147483647), ('KILL_AFTER_SECONDS', 30, 1, 2147483647))}
    c.update(TASKS_PER_THREAD=16, PROFILES=PROFILES, SOURCE_DIR=REPO,
             PERSISTENCE='pmem_header_plus_32B_value',
             FLUSH_THRESHOLDS={str(p[0]): dict(flush_block_count=p[5],
                 flush_threshold_bytes=p[5] * BLOCK_SIZE) for p in PROFILES})
    if c['THREADS'] + c['LOAD_INJECTORS'] + 1 > 128:
        raise ValueError('threads + injectors + 1 must not exceed 128')
    c['LIBDB_ALLOW_DESTRUCTIVE'] = os.environ.get('LIBDB_ALLOW_DESTRUCTIVE', '1')
    if c['LIBDB_ALLOW_DESTRUCTIVE'] != '1':
        raise ValueError('set LIBDB_ALLOW_DESTRUCTIVE=1 for the disposable experiment SSD')
    c['PCI_ADDR'] = os.environ.get('PCI_ADDR', '0000:5c:00.0')
    if not re.fullmatch(r'[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[01][0-9a-fA-F]\.[0-7]', c['PCI_ADDR']):
        raise ValueError('PCI_ADDR must identify the dedicated SSD as DDDD:BB:DD.F')
    for tool in ('cmake', 'stdbuf'):
        c[tool.upper()] = shutil.which(tool)
        if not c[tool.upper()]:
            raise ValueError(f'{tool} is required')
    c['SPDK_ROOT'] = existing('SPDK_ROOT', REPO.parent / 'spdk', directory=True)
    if os.environ.get('PMEM_BASE') == '':
        raise ValueError('PMEM_BASE must not be empty')
    c['PMEM_BASE'] = existing('PMEM_BASE', '/mnt/whypmem', directory=True)
    if c['PMEM_BASE'] == Path('/'):
        raise ValueError('PMEM_BASE must not be /')
    c['RESULT_BASE'] = Path(os.environ.get('RESULT_BASE', REPO / 'results')).resolve()
    root = Path(os.environ.get('WORKLOAD_ROOT', '/mnt/nvme0n1/why/ycsb_workloads_one_warm'))
    input_keys = []
    for dist in ('uniform', 'zipfian'):
        for kind, filename in (('WARM', 'warm.txt'), ('YCSBA', 'ycsba.txt')):
            key = f'{dist.upper()}_{kind}_FILE'
            input_keys.append(key)
            c[key] = existing(key, root / dist / filename)
            if 'Production' in str(c[key]):
                raise ValueError(f'{key}: Production paths select a different database reader')
    if not c['SKIP_DRIVER_CHECK']:
        driver = Path('/sys/bus/pci/devices') / c['PCI_ADDR'] / 'driver'
        if not driver.is_symlink() or driver.resolve().name not in ('uio_pci_generic', 'vfio-pci'):
            raise ValueError(f'{c["PCI_ADDR"]}: expected an already bound uio_pci_generic or vfio-pci driver')
        c['PCI_DRIVER'] = driver.resolve().name
    else:
        c['PCI_DRIVER'] = 'check_skipped'
    c['ROOT_COMMAND'] = []
    if c['USE_SUDO'] and os.geteuid() != 0:
        if not shutil.which('sudo'):
            raise ValueError('sudo is required')
        subprocess.run(['sudo', '-v'], check=True)
        c['ROOT_COMMAND'] = ['sudo', '-n']
    for permission in ('-w', '-x'):
        subprocess.run(c['ROOT_COMMAND'] + ['test', permission, str(c['PMEM_BASE'])], check=True)
    # Validate all four inputs before building or touching experiment storage.
    counts = {}
    for key in input_keys:
        print(f'Checking input: {c[key]}', flush=True)
        if c[key] not in counts:
            counts[c[key]] = count_requests(c[key])
        c[key + '_REQUESTS'] = counts[c[key]]
    return c



def run(c):
    c['RESULT_BASE'].mkdir(parents=True, exist_ok=True)
    result = Path(tempfile.mkdtemp(prefix=time.strftime('ycsb_ae_kvsize_%Y%m%d_%H%M%S_'),
                                   dir=c['RESULT_BASE']))
    print(f'Results: {result}', flush=True)
    summary = result / 'summary.csv'
    with summary.open('x', newline='') as stream:
        csv.DictWriter(stream, fieldnames=FIELDS).writeheader()
    c['BINARIES'] = {str(p[0]): result / 'build' / str(p[0]) / 'test' for p in PROFILES}
    save_json(result / 'config.json', c)
    status = dict(status='running', stage='build', error='')
    pmem = None
    try:
        build_control = dict(c, ROOT_COMMAND=[])
        for kv, cache, hard, high, low, flush_blocks in PROFILES:
            build_dir = c['BINARIES'][str(kv)].parent
            flags = {'LIBDB_DYNAMIC_FLUSH_ENABLED': 1,
                     'LIBDB_FLUSH_BLOCK_COUNT': flush_blocks,
                     'LIBDB_WBF_CAPACITY_LIMIT_BYTES': f'{hard * GIB}ULL',
                     'LIBDB_NVM_FLUSH_HIGH_BYTES': f'{high * GIB}ULL',
                     'LIBDB_NVM_FLUSH_LOW_BYTES': f'{low * GIB}ULL',
                     'LIBDB_NVM_FLUSH_POLL_NS': '100000000ULL'}
            commands = (
                ('configure', [c['CMAKE'], '-S', str(REPO), '-B', str(build_dir),
                    '-DBUILD_TESTING=OFF', f'-DSPDK_ROOT={c["SPDK_ROOT"]}',
                    '-DCMAKE_C_FLAGS=' + ' '.join(f'-D{k}={v}' for k, v in flags.items())]),
                ('build', [c['CMAKE'], '--build', str(build_dir), '--target', 'libdb',
                    '--parallel', str(c['BUILD_JOBS'])]))
            for step, command in commands:
                status['stage'] = f'{step}_{kv}'
                print(f'{step.capitalize()} KVsize={kv}', flush=True)
                save_json(result / f'{step}_{kv}.command.json', command)
                execute(command, result / f'{step}_{kv}.log', build_control)
            binary = c['BINARIES'][str(kv)]
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise ValueError(f'build did not produce executable {binary}')
        status['stage'] = 'prepare_pmem'
        pmem = Path(subprocess.check_output(c['ROOT_COMMAND'] + ['mktemp', '-d',
            str(c['PMEM_BASE'] / 'libdb_ycsb_ae_kvsize_XXXXXXXXXX')], text=True).strip())
        c['PMEM_DIR'] = pmem
        save_json(result / 'config.json', c)
        for repeat in range(1, c['REPEATS'] + 1):
            for profile in PROFILES:
                kv, cache, hard, high, low, flush_blocks = profile
                for dist, kind in CASES:
                    label = f'{repeat:02d}_kv{kv}_{dist}_{kind}'
                    status['stage'] = label
                    key = f'{dist.upper()}_YCSB{kind}_FILE'
                    row = dict(round=repeat, kv_size_bytes=kv, cache_mib=cache,
                        flush_block_count=flush_blocks, flush_threshold_bytes=flush_blocks * BLOCK_SIZE,
                        distribution=dist, workload=kind, workers=c['THREADS'],
                        injectors=c['LOAD_INJECTORS'], tasks_per_thread=16,
                        persistence=c['PERSISTENCE'], dynamic_flush_enabled=1,
                        requests=c[key + '_REQUESTS'], precondition_file='', precondition_requests=0,
                        nvm_limit_bytes=high * GIB, nvm_low_bytes=low * GIB,
                        nvm_hard_limit_bytes=hard * GIB, nvm_poll_ns=100000000,
                        wbf_file_peak_scope='whole_run', status='prepare_failed', error='')
                    log = result / f'{label}.log'
                    try:
                        database = pmem / label
                        subprocess.run(c['ROOT_COMMAND'] + ['mkdir', '--', str(database)], check=True)
                        args = {'thread': c['THREADS'], 'load-injectors': c['LOAD_INJECTORS'],
                            'kvsize': kv, 'kvs': c['KV_COUNT'], 'cachesize': cache,
                            'tasks-per-thread': 16, 'boundary-size': c['BOUNDARY_SIZE'],
                            'pci-addr': c['PCI_ADDR'], 'pmem-dir': database,
                            'warm': c[f'{dist.upper()}_WARM_FILE'], 'test': c[key],
                            'output': result / f'{label}.result.txt'}
                        command = c['ROOT_COMMAND'] + [c['STDBUF'], '-oL', '-eL', str(c['BINARIES'][str(kv)])]
                        command += [f'--{k}={v}' for k, v in args.items()]
                        save_json(result / f'{label}.command.json', command)
                        row['status'] = 'failed'
                        print(f'Starting {label} (fresh database)', flush=True)
                        execute(command, log, c)
                        row['status'] = 'invalid_result'
                        row.update(measured_result(log, args['output'], workload=kind,
                            test_file=c[key], expected=row['requests'],
                            warm_expected=c[f'{dist.upper()}_WARM_FILE_REQUESTS'], profile=profile))
                        row['status'] = 'ok'
                    except Exception as error:
                        if log.exists():
                            with log.open() as stream:
                                for line in stream:
                                    match = NVM_REJECTION.search(line)
                                    if match:
                                        size, requested, limit, rejected = map(int, match.groups())
                                        row.update(wbf_file_bytes=size, wbf_limit_rejections=rejected,
                                            wbf_capacity_limit_hit=1, status='capacity_limit')
                        if isinstance(error, Interrupted): row['status'] = 'interrupted'
                        elif isinstance(error, TimeoutError): row['status'] = 'timeout'
                        row['error'] = str(error)
                        raise
                    finally:
                        with summary.open('a', newline='') as stream:
                            csv.DictWriter(stream, fieldnames=FIELDS).writerow(row)
                        aggregate(summary, result / 'aggregate.csv')
                    subprocess.run(c['ROOT_COMMAND'] + ['rm', '-rf', '--', str(database)], check=True)
        print((result / 'aggregate.csv').read_text(), end='', flush=True)
        status.update(status='ok', stage='complete')
    except Exception as error:
        status.update(status='interrupted' if isinstance(error, Interrupted) else
                      'timeout' if isinstance(error, TimeoutError) else 'failed', error=str(error))
        raise
    finally:
        try:
            if pmem is not None and not c.get('_PROCESS_UNSTOPPED'):
                subprocess.run(c['ROOT_COMMAND'] + ['rm', '-rf', '--', str(pmem)], check=True)
            if c.get('_PROCESS_UNSTOPPED'):
                print(f'error: database could not be stopped; PMEM preserved at {pmem}', file=sys.stderr)
        except Exception as error:
            status.update(status='cleanup_failed', error=str(error))
            raise
        finally:
            aggregate(summary, result / 'aggregate.csv')
            save_json(result / 'status.json', status)
            print(f'Results and logs retained: {result}', flush=True)


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


if __name__ == '__main__': sys.exit(main())
