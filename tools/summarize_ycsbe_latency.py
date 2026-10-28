#!/usr/bin/env python3
"""Validate measured E-stage latency and summarize independent runs."""
import argparse
import csv
import math
from pathlib import Path
import re
import statistics
from summarize_ycsba import NUMBER, RATE, STATS

LATENCY_FIELDS = ['avg_latency_us', 'p50_us', 'p95_us', 'p99_us', 'p999_us', 'max_latency_us']
METRICS = ['throughput_ops_s'] + LATENCY_FIELDS
FIELDS = ['distribution', 'workload', 'round', 'workers', 'injectors', 'tasks_per_thread',
          'kv_size_bytes', 'cache_mib', 'requests', 'seconds'] + METRICS + ['status', 'error']
PERCENTILE = re.compile(rf'\s*({NUMBER})%\s*:\s*({NUMBER})us\s*')
AVERAGE = re.compile(rf'平均延迟:\s*({NUMBER})us\s*')
MAXIMUM = re.compile(rf'最大延迟:\s*({NUMBER})us\s*')


def latency_result(log, test_file):
    started = False
    rate_seen = False
    values, percentiles = {}, {}
    with open(log) as stream:
        for raw in stream:
            line = raw.rstrip('\n')
            if line == f'start to read file {test_file}':
                if started: raise ValueError('duplicate measured phase')
                started = True
                continue
            if not started: continue
            if RATE.fullmatch(line):
                if rate_seen: raise ValueError('duplicate measured throughput')
                rate_seen = True
                continue
            percentile = PERCENTILE.fullmatch(line)
            average = AVERAGE.fullmatch(line)
            maximum = MAXIMUM.fullmatch(line)
            if not (percentile or average or maximum):
                if re.match(r'\s*(?:\S+%\s*:|平均延迟:|最大延迟:)', line):
                    raise ValueError('malformed measured latency statistic')
                continue
            if not rate_seen: raise ValueError('measured histogram precedes throughput')
            if percentile:
                cutoff, value = map(float, percentile.groups())
                if cutoff in percentiles: raise ValueError('duplicate measured percentile')
                if not 0 < cutoff <= 100: raise ValueError('invalid percentile')
                percentiles[cutoff] = value
            else:
                name, match = ('avg_latency_us', average) if average else ('max_latency_us', maximum)
                if name in values: raise ValueError('duplicate measured latency statistic')
                values[name] = float(match[1])
    for cutoff, name in ((50, 'p50_us'), (95, 'p95_us'), (99, 'p99_us'), (99.9, 'p999_us')):
        if cutoff not in percentiles: raise ValueError(f'missing measured percentile {cutoff}')
        values[name] = percentiles[cutoff]
    if set(values) != set(LATENCY_FIELDS): raise ValueError('missing measured average or maximum')
    if any(not math.isfinite(v) or v < 0 for v in [*values.values(), *percentiles.values()]):
        raise ValueError('invalid measured latency')
    ordered = [percentiles[k] for k in sorted(percentiles)]
    if ordered != sorted(ordered) or max(ordered) > values['max_latency_us'] or values['avg_latency_us'] > values['max_latency_us']:
        raise ValueError('inconsistent measured latency ordering')
    return values


def measured_result(log, result, test_file, expected, warm_expected):
    start = f'start to read file {test_file}'
    read_done = re.compile(re.escape(f'read file {test_file} ') + r'(\d+) request done')
    starts, reads, rates = 0, [], []
    with open(log) as stream:
        for raw in stream:
            line = raw.rstrip('\n')
            if line == start:
                starts += 1
            match = read_done.fullmatch(line)
            if match:
                if starts != 1:
                    raise ValueError('test read completion is out of order')
                reads.append(int(match[1]))
            match = RATE.fullmatch(line)
            if match and starts:
                if len(reads) != 1:
                    raise ValueError('test throughput precedes read completion')
                rates.append(match.groups())
    if starts != 1 or reads != [expected] or len(rates) != 1:
        raise ValueError('missing, duplicate or truncated measured phase in log')
    count, seconds, throughput = rates[0]
    elapsed, rate = float(seconds), float(throughput)
    if int(count) != expected or not all(math.isfinite(x) and x > 0 for x in (elapsed, rate)):
        raise ValueError('invalid measured count, elapsed time or throughput')
    stats = []
    with open(result) as stream:
        for line in stream:
            match = STATS.fullmatch(line.rstrip('\n'))
            if match:
                stats.append(match.groups())
    if len(stats) != 3:
        raise ValueError('expected load, cache warmup and measured statistics')
    if [int(row[0]) for row in stats] != [warm_expected, warm_expected // 2, expected]:
        raise ValueError('load, warmup or measured request count mismatch')
    final = stats[-1]
    actual_ops = list(map(int, final[1:5]))
    if sum(actual_ops) != expected or actual_ops[1] or actual_ops[2] or not actual_ops[3]:
        raise ValueError('E must contain INSERT/SCAN requests, including scans')
    if not math.isclose(float(final[5]), elapsed, rel_tol=0, abs_tol=1e-6):
        raise ValueError('log and result elapsed times disagree')
    tolerance = max(1e-5, rate * 1e-6 / elapsed)
    if abs(expected / elapsed - rate) > tolerance:
        raise ValueError('reported throughput does not match count and elapsed time')
    metrics = latency_result(log, test_file)
    metrics.update(seconds=seconds, throughput_ops_s=throughput)
    return metrics


def aggregate(summary, destination):
    with open(summary, newline='') as stream:
        rows = [r for r in csv.DictReader(stream) if r['status'] == 'ok']
    fields = ['distribution', 'workload', 'tasks_per_thread', 'successful_runs']
    fields += [f'{metric}_{stat}' for metric in METRICS for stat in ('mean', 'stddev')]
    output = dict(distribution='uniform', workload='E', tasks_per_thread=2, successful_runs=len(rows))
    for metric in METRICS:
        values = [float(r[metric]) for r in rows]
        output[metric + '_mean'] = statistics.mean(values) if values else ''
        output[metric + '_stddev'] = statistics.stdev(values) if len(values) > 1 else (0 if values else '')
    with open(destination, 'w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerow(output)


def main():
    parser = argparse.ArgumentParser(description='Aggregate E latency across runs; percentile means are NOT pooled percentiles.')
    parser.add_argument('summary', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    destination = args.output or args.summary.with_name('aggregate.csv')
    aggregate(args.summary, destination)
    print(destination.read_text(), end='')


if __name__ == '__main__': main()
