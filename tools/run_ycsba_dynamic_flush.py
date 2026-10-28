#!/usr/bin/env python3
"""Build and compare dynamic Flush OFF then ON, one fresh uniform YCSB A run each."""
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

from run_ycsba_uniform_zipfian import Interrupted, interrupt, positive, existing, execute
from summarize_ycsba import count_requests, measured_result

REPO = Path(__file__).resolve().parent.parent
MODES = (('off', 0), ('on', 1))
FIELDS = ['mode', 'dynamic_flush_enabled', 'distribution', 'workload', 'workers',
          'injectors', 'kv_size_bytes', 'cache_mib', 'tasks_per_thread', 'requests',
          'seconds', 'throughput_ops_s', 'nvm_limit_bytes', 'nvm_poll_ns',
          'nvm_used_slot_bytes', 'nvm_sampled_peak_bytes', 'nvm_pressure_changes',
          'nvm_sampled', 'status', 'error']
NVM_CONFIG = re.compile(r'NVM flush: enabled=([01]) limit_bytes=(\d+) poll_ns=(\d+) '
                        r'occupancy=unreclaimed_slots phase=workload_only soft_limit=1')
NVM_STATS = re.compile(r'nvm_used_slot_bytes=(\d+) nvm_sampled_peak_bytes=(\d+) '
                       r'nvm_pressure_changes=(\d+) nvm_sampled=([01])')


def help_parser():
    return argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog='''
Run directly: sudo ./run_ycsba_dynamic_flush.sh
Environment defaults:
  PCI_ADDR=0000:5c:00.0 PMEM_BASE=/mnt/whypmem LIBDB_ALLOW_DESTRUCTIVE=1
  WORKLOAD_ROOT=/mnt/nvme0n1/why/ycsb_workloads_one_warm
  UNIFORM_WARM_FILE=$WORKLOAD_ROOT/uniform/warm.txt
  UNIFORM_YCSBA_FILE=$WORKLOAD_ROOT/uniform/ycsba.txt
  THREADS=16 LOAD_INJECTORS=8 KV_SIZE=1024 KV_COUNT=100000000
  CACHE_SIZE_MB=20000 TASKS_PER_THREAD=16 BOUNDARY_SIZE=1000
  BUILD_JOBS=6 SPDK_ROOT=<repo>/../spdk RESULT_BASE=<repo>/results USE_SUDO=1
  TIMEOUT_SECONDS=0 KILL_AFTER_SECONDS=30
  SKIP_DRIVER_CHECK=0 (1 is intended only for mock-program tests)
Always runs OFF once, then ON once. REPEATS and BINARY are ignored.
Both binaries are built in this invocation's result directory before either run.
Only LIBDB_DYNAMIC_FLUSH_ENABLED differs; other settings come from current source.
Requires cmake and stdbuf. Build/runtime stdout and stderr appear live in this
terminal and are also saved to logs. TIMEOUT_SECONDS applies to each configure,
build or database process (0 means unlimited).
No workload generation, CPU/NUMA tuning or NVMe driver changes. Only the newly
created private PMEM child is removed. Results, binaries and build trees remain.
''')


def configuration():
    c = {name: positive(name, default, minimum, maximum) for name, default, minimum, maximum in (
        ('THREADS', 16, 1, 126), ('LOAD_INJECTORS', 8, 1, 126),
        ('KV_SIZE', 1024, 52, 32768), ('KV_COUNT', 100000000, 1, 2147483647),
        ('CACHE_SIZE_MB', 20000, 0, 2147483647), ('TASKS_PER_THREAD', 16, 1, 2147483647),
        ('BOUNDARY_SIZE', 1000, 1, 2147483647), ('BUILD_JOBS', 6, 1, 1024),
        ('USE_SUDO', 1, 0, 1), ('SKIP_DRIVER_CHECK', 0, 0, 1),
        ('TIMEOUT_SECONDS', 0, 0, 2147483647), ('KILL_AFTER_SECONDS', 30, 1, 2147483647))}
    c['MODES'] = MODES
    c['REPEATS'] = 1
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
    for kind, filename in (('WARM', 'warm.txt'), ('YCSBA', 'ycsba.txt')):
        key = f'UNIFORM_{kind}_FILE'
        c[key] = existing(key, root / 'uniform' / filename)
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
    # Validate both inputs before building or creating experiment storage.
    counts = {}
    for key in ('UNIFORM_WARM_FILE', 'UNIFORM_YCSBA_FILE'):
        print(f'Checking input: {c[key]}', flush=True)
        if c[key] not in counts:
            counts[c[key]] = count_requests(c[key])
        c[key + '_REQUESTS'] = counts[c[key]]
    return c


def save_json(path, value):
    path.write_text(json.dumps(value, default=str, indent=2) + '\n')


def nvm_metrics(log, result, enabled):
    """Require a final snapshot in both streams, and validate every mode line."""
    snapshots = []
    geometry = None
    for path in (log, result):
        configs, stats = [], []
        with path.open() as stream:
            for line in stream:
                match = NVM_CONFIG.fullmatch(line.rstrip('\n'))
                if match:
                    configs.append(tuple(map(int, match.groups())))
                match = NVM_STATS.fullmatch(line.rstrip('\n'))
                if match:
                    stats.append(tuple(map(int, match.groups())))
        if not configs or not stats:
            raise ValueError(f'{path.name}: missing NVM flush configuration or final statistics')
        for mode, limit, interval in configs:
            if mode != enabled:
                raise ValueError(f'{path.name}: dynamic Flush mode mismatch: expected {enabled}, got {mode}')
            if not limit or not interval:
                raise ValueError('invalid NVM flush limit or poll interval')
            if geometry is not None and geometry != (limit, interval):
                raise ValueError('inconsistent NVM flush geometry')
            geometry = limit, interval
        if stats[-1][-1] != enabled:
            raise ValueError('NVM sampling state does not match the measured mode')
        snapshots.append(stats[-1])
    if snapshots[0] != snapshots[1]:
        raise ValueError('log and result NVM snapshots disagree')
    return dict(zip(('nvm_limit_bytes', 'nvm_poll_ns', 'nvm_used_slot_bytes',
                     'nvm_sampled_peak_bytes', 'nvm_pressure_changes', 'nvm_sampled'),
                    geometry + snapshots[0]))


def run(c):
    c['RESULT_BASE'].mkdir(parents=True, exist_ok=True)
    result = Path(tempfile.mkdtemp(prefix=time.strftime('ycsba_dynamic_flush_%Y%m%d_%H%M%S_'),
                                   dir=c['RESULT_BASE']))
    print(f'Results: {result}', flush=True)
    summary = result / 'summary.csv'
    with summary.open('x', newline='') as stream:
        csv.DictWriter(stream, fieldnames=FIELDS).writeheader()
    c['BINARIES'] = {mode: result / 'build' / mode / 'test' for mode, _ in MODES}
    save_json(result / 'config.json', c)
    status = dict(status='running', stage='build', error='')
    pmem = None
    rows = []
    try:
        # Builds are unprivileged when the script itself is unprivileged. Their
        # process groups use the same streaming/timeout handler as the database.
        build_control = dict(c, ROOT_COMMAND=[])
        for mode, enabled in MODES:
            build_dir = c['BINARIES'][mode].parent
            commands = (
                ('configure', [c['CMAKE'], '-S', str(REPO), '-B', str(build_dir),
                    '-DBUILD_TESTING=OFF', f'-DSPDK_ROOT={c["SPDK_ROOT"]}',
                    f'-DCMAKE_C_FLAGS=-DLIBDB_DYNAMIC_FLUSH_ENABLED={enabled}']),
                ('build', [c['CMAKE'], '--build', str(build_dir), '--target', 'libdb',
                    '--parallel', str(c['BUILD_JOBS'])]))
            for step, command in commands:
                status['stage'] = f'{step}_{mode}'
                print(f'{step.capitalize()} dynamic Flush {mode.upper()}', flush=True)
                save_json(result / f'{step}_{mode}.command.json', command)
                execute(command, result / f'{step}_{mode}.log', build_control)
            if not c['BINARIES'][mode].is_file() or not os.access(c['BINARIES'][mode], os.X_OK):
                raise ValueError(f'build did not produce executable {c["BINARIES"][mode]}')
        # No experimental PMEM or SSD access until both builds succeed.
        status['stage'] = 'prepare_pmem'
        pmem = Path(subprocess.check_output(c['ROOT_COMMAND'] + ['mktemp', '-d',
            str(c['PMEM_BASE'] / 'libdb_ycsba_dynamic_flush_XXXXXXXXXX')], text=True).strip())
        c['PMEM_DIR'] = pmem
        save_json(result / 'config.json', c)
        for mode, enabled in MODES:
            label = f'{mode}_uniform_A'
            status['stage'] = label
            row = dict(mode=mode, dynamic_flush_enabled=enabled, distribution='uniform', workload='A',
                workers=c['THREADS'], injectors=c['LOAD_INJECTORS'], kv_size_bytes=c['KV_SIZE'],
                cache_mib=c['CACHE_SIZE_MB'], tasks_per_thread=c['TASKS_PER_THREAD'],
                requests=c['UNIFORM_YCSBA_FILE_REQUESTS'], status='prepare_failed', error='')
            try:
                database = pmem / mode
                subprocess.run(c['ROOT_COMMAND'] + ['mkdir', '--', str(database)], check=True)
                args = {'thread': c['THREADS'], 'load-injectors': c['LOAD_INJECTORS'],
                    'kvsize': c['KV_SIZE'], 'kvs': c['KV_COUNT'], 'cachesize': c['CACHE_SIZE_MB'],
                    'tasks-per-thread': c['TASKS_PER_THREAD'], 'boundary-size': c['BOUNDARY_SIZE'],
                    'pci-addr': c['PCI_ADDR'], 'pmem-dir': database, 'warm': c['UNIFORM_WARM_FILE'],
                    'test': c['UNIFORM_YCSBA_FILE'], 'output': result / f'{label}.result.txt'}
                command = c['ROOT_COMMAND'] + [c['STDBUF'], '-oL', '-eL', str(c['BINARIES'][mode])]
                command += [f'--{k}={v}' for k, v in args.items()]
                save_json(result / f'{label}.command.json', command)
                row['status'] = 'failed'
                print(f'Starting uniform YCSB A: dynamic Flush {mode.upper()} (fresh database)', flush=True)
                log = result / f'{label}.log'
                execute(command, log, c)
                row['status'] = 'invalid_result'
                seconds, rate = measured_result(log, args['output'], str(args['test']),
                    row['requests'], c['UNIFORM_WARM_FILE_REQUESTS'])
                metrics = nvm_metrics(log, args['output'], enabled)
                if rows and any(metrics[k] != rows[0][k] for k in ('nvm_limit_bytes', 'nvm_poll_ns')):
                    raise ValueError('OFF and ON use different NVM limits or sampling intervals')
                row.update(metrics, seconds=seconds, throughput_ops_s=rate, status='ok')
            except Exception as error:
                if isinstance(error, Interrupted): row['status'] = 'interrupted'
                elif isinstance(error, TimeoutError): row['status'] = 'timeout'
                row['error'] = str(error)
                raise
            finally:
                with summary.open('a', newline='') as stream:
                    csv.DictWriter(stream, fieldnames=FIELDS).writerow(row)
                rows.append(row)
            # Reclaim the completed phase before the next one so the two PMEM
            # populations do not compete for capacity. Both paths are private.
            subprocess.run(c['ROOT_COMMAND'] + ['rm', '-rf', '--', str(database)], check=True)
        off, on = (float(row['throughput_ops_s']) for row in rows)
        comparison = (f'Uniform YCSB A: OFF={off:.6f} ops/s, ON={on:.6f} ops/s\n'
                      f'ON relative to OFF: {(on / off - 1) * 100:+.2f}%\n')
        (result / 'comparison.txt').write_text(comparison)
        print(summary.read_text(), end='', flush=True)
        print(comparison, end='', flush=True)
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


if __name__ == '__main__':
    sys.exit(main())
