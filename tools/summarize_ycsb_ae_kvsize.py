#!/usr/bin/env python3
"""Validate phase-delimited A/E results and aggregate the KV-size matrix."""
import argparse
import csv
import math
from pathlib import Path
import re
import statistics

from summarize_ycsba import RATE, STATS
from run_ycsba_dynamic_flush import nvm_metrics, NVM_CONFIG_V2, NVM_STATS

GIB = 1024 ** 3
BLOCK_SIZE = 4096
# KV bytes, cache MiB, WBF hard/high/low GiB, local Flush threshold blocks.
PROFILES = ((1024, 20480, 16, 14, 12, 4), (512, 10240, 8, 7, 6, 2),
            (256, 5120, 4, 3, 2, 1), (128, 1024, 4, 3, 2, 1))
CASES = (('uniform', 'A'), ('zipfian', 'A'))
FIELDS = ['round', 'kv_size_bytes', 'cache_mib', 'flush_block_count', 'flush_threshold_bytes', 'distribution', 'workload',
          'workers', 'injectors', 'tasks_per_thread', 'persistence',
          'dynamic_flush_enabled', 'requests', 'precondition_file', 'precondition_requests',
          'seconds', 'throughput_ops_s', 'nvm_limit_bytes', 'nvm_low_bytes',
          'nvm_hard_limit_bytes', 'nvm_poll_ns', 'nvm_used_slot_bytes',
          'nvm_sampled_peak_bytes', 'nvm_pressure_changes', 'nvm_sampled',
          'wbf_file_count', 'wbf_file_bytes', 'wbf_file_peak_bytes',
          'wbf_file_peak_scope', 'wbf_reusable_slot_bytes', 'nvm_pressure_bytes',
          'nvm_pressure_sampled_peak_bytes', 'wbf_limit_rejections',
          'wbf_capacity_verified', 'wbf_capacity_limit_hit', 'status', 'error']
MARKER = re.compile(r'Benchmark phase: (begin|end) (load|warmup|precondition|measured)')


def phase_data(path, phases, traces):
    data, active, seen = {}, None, []
    with path.open() as stream:
        for raw in stream:
            line = raw.rstrip('\n')
            marker = MARKER.fullmatch(line)
            if marker:
                event, name = marker.groups()
                if event == 'begin':
                    if active or len(seen) >= len(phases) or name != phases[len(seen)]:
                        raise ValueError(f'{path.name}: unexpected phase begin: {name}')
                    active = name
                    data[name] = dict(rates=[], stats=[], starts=0, reads=[], nvm=[], config=[])
                    seen.append(name)
                else:
                    if active != name:
                        raise ValueError(f'{path.name}: unexpected phase end: {name}')
                    active = None
                continue
            if line.startswith('Benchmark phase:'):
                raise ValueError(f'{path.name}: malformed phase marker')
            rate, stat = RATE.fullmatch(line), STATS.fullmatch(line)
            if not active:
                if rate or stat:
                    raise ValueError(f'{path.name}: unscoped throughput/statistics')
                continue
            current = data[active]
            if active in traces:
                trace = str(traces[active])
                if line == f'start to read file {trace}':
                    if current['reads'] or current['rates']:
                        raise ValueError('out-of-order workload read')
                    current['starts'] += 1
                match = re.fullmatch(re.escape(f'read file {trace} ') + r'(\d+) request done', line)
                if match:
                    if current['starts'] != 1 or current['rates']:
                        raise ValueError('out-of-order workload read completion')
                    current['reads'].append(int(match[1]))
                if rate and (current['starts'] != 1 or len(current['reads']) != 1):
                    raise ValueError('throughput precedes trace read completion')
            if rate: current['rates'].append(rate.groups())
            if stat: current['stats'].append(stat.groups())
            nvm, config = NVM_STATS.fullmatch(line), NVM_CONFIG_V2.fullmatch(line)
            if nvm: current['nvm'].append(tuple(map(int, nvm.groups())))
            if config: current['config'].append(tuple(map(int, config.groups())))
    if active or seen != phases:
        raise ValueError(f'{path.name}: missing or unfinished benchmark phase')
    return data


def flush_metrics(result, expected):
    counts = []
    with result.open() as stream:
        for line in stream:
            if line.startswith('LIBDB_FLUSH_BLOCK_COUNT='):
                match = re.fullmatch(r'LIBDB_FLUSH_BLOCK_COUNT=([0-9]+)\n?', line)
                if not match:
                    raise ValueError('malformed LIBDB_FLUSH_BLOCK_COUNT in result')
                counts.append(int(match[1]))
    if counts != [expected]:
        raise ValueError(f'LIBDB_FLUSH_BLOCK_COUNT: expected one value {expected}, got {counts}')
    return dict(flush_block_count=expected, flush_threshold_bytes=expected * BLOCK_SIZE)


def measured_result(log, result, *, workload, test_file, expected, warm_expected,
                    profile, precondition_file=None, precondition_expected=0):
    phases = ['load', 'warmup'] + (['precondition'] if workload == 'E' else []) + ['measured']
    if workload not in ('A', 'E') or (workload == 'E' and not precondition_file):
        raise ValueError('E requires a uniform A precondition trace')
    traces = {'measured': test_file}
    counts = dict(load=warm_expected, warmup=warm_expected // 2, measured=expected)
    if workload == 'E':
        traces['precondition'] = precondition_file
        counts['precondition'] = precondition_expected
    logs, results = phase_data(log, phases, traces), phase_data(result, phases, {})
    _, _, hard, high, low, flush_blocks = profile
    flush = flush_metrics(result, flush_blocks)
    geometry = (1, high * GIB, 100000000, low * GIB, hard * GIB)
    for phase in phases:
        a, b = logs[phase], results[phase]
        if len(a['rates']) != 1 or len(b['stats']) != 1:
            raise ValueError(f'{phase}: missing or duplicate throughput/statistics')
        count, seconds, rate = a['rates'][0]
        stat = b['stats'][0]
        elapsed, throughput = float(seconds), float(rate)
        if int(count) != counts[phase] or int(stat[0]) != counts[phase]:
            raise ValueError(f'{phase}: request count mismatch')
        if not all(math.isfinite(v) and v > 0 for v in (elapsed, throughput)):
            raise ValueError(f'{phase}: invalid throughput or elapsed time')
        if not math.isclose(float(stat[5]), elapsed, rel_tol=0, abs_tol=1e-6):
            raise ValueError(f'{phase}: log/result time mismatch')
        if abs(counts[phase] / elapsed - throughput) > max(1e-5, throughput * 1e-6 / elapsed):
            raise ValueError(f'{phase}: throughput does not match count/time')
        if phase in traces:
            if a['starts'] != 1 or a['reads'] != [counts[phase]]:
                raise ValueError(f'{phase}: missing, duplicate or truncated trace')
            insert, update, get, scan = map(int, stat[1:5])
            if insert + update + get + scan != counts[phase]:
                raise ValueError(f'{phase}: operation count mismatch')
            if phase == 'precondition' or workload == 'A':
                if insert or scan:
                    raise ValueError(f'{phase}: A requires READ/UPDATE operations')
            elif update or get or not scan:
                raise ValueError('E requires INSERT/SCAN operations, including scans')
            if a['config'] != [geometry] or b['config'] != [geometry]:
                raise ValueError(f'{phase}: dynamic Flush geometry mismatch')
            if len(a['nvm']) != 1 or a['nvm'] != b['nvm'] or a['nvm'][0][-1] != 1:
                raise ValueError(f'{phase}: missing/inconsistent dynamic Flush sampling')
    metrics = nvm_metrics(log, result, 1)
    if (metrics['nvm_limit_bytes'], metrics['nvm_low_bytes'],
        metrics['nvm_hard_limit_bytes'], metrics['nvm_poll_ns']) != (high * GIB, low * GIB, hard * GIB, 100000000):
        raise ValueError('final dynamic Flush geometry mismatch')
    final_sample = tuple(metrics[k] for k in ('nvm_sampled_peak_bytes',
                                             'nvm_pressure_changes', 'nvm_sampled'))
    if final_sample != logs['measured']['nvm'][0][1:]:
        raise ValueError('final sampling does not belong to measured phase')
    if not metrics['wbf_capacity_verified'] or metrics['wbf_capacity_limit_hit']:
        raise ValueError('WBF hard capacity missing or rejected allocation')
    _, seconds, rate = logs['measured']['rates'][0]
    return dict(metrics, **flush, seconds=seconds, throughput_ops_s=rate)


def aggregate(summary, destination):
    with summary.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    # Older CSVs have no local-threshold evidence; keep those groups unknown.
    for row in rows:
        row.setdefault('flush_block_count', '')
        row.setdefault('flush_threshold_bytes', '')
    keys = ['kv_size_bytes', 'cache_mib', 'flush_block_count', 'flush_threshold_bytes', 'distribution', 'workload', 'tasks_per_thread',
            'nvm_hard_limit_bytes', 'nvm_limit_bytes', 'nvm_low_bytes']
    fields = keys + ['successful_runs', 'mean_ops_s', 'stddev_ops_s', 'min_ops_s', 'max_ops_s']
    with destination.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for group in dict.fromkeys(tuple(row[k] for k in keys) for row in rows):
            rates = [float(row['throughput_ops_s']) for row in rows
                     if row['status'] == 'ok' and tuple(row[k] for k in keys) == group]
            writer.writerow(dict(zip(fields, [*group, len(rates),
                statistics.mean(rates) if rates else '',
                statistics.stdev(rates) if len(rates) > 1 else (0 if rates else ''),
                min(rates) if rates else '', max(rates) if rates else ''])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('summary', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    destination = args.output or args.summary.with_name('aggregate.csv')
    aggregate(args.summary, destination)
    print(destination.read_text(), end='')


if __name__ == '__main__': main()
