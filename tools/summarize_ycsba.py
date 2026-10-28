#!/usr/bin/env python3
"""Validate LibDB's measured phase and write an incremental CSV summary."""
import argparse
import csv
import math
from pathlib import Path
import re
import sys

NUMBER = r'[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?'
RATE = re.compile(rf'(\d+) ops , time is ({NUMBER}) , throughput is ({NUMBER})')
STATS = re.compile(rf'test:total (\d+) ops Insert (\d+) Update (\d+) Get (\d+) Scan (\d+) time ({NUMBER})s')


def count_requests(path):
    count, last = 0, b''
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            count += chunk.count(b'\n')
            last = chunk[-1:]
    count += bool(last and last != b'\n')
    # The existing reader truncates to thousands and stores the count in int.
    if count < 1000 or count % 1000 or count > 2147483647:
        raise ValueError(f'{path}: {count} records; require a positive multiple of 1000 within INT_MAX to avoid truncation')
    return count


def inspect_input(path, *, is_f=False):
    """Count input and describe the suffix parsed but not executed by client.c."""
    tail_ops = dict.fromkeys(('I', 'U', 'R', 'S'), 0)
    if not is_f:
        raw = count_requests(path)
    else:
        raw, last = 0, b''
        with open(path, 'rb') as stream:
            for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
                raw += chunk.count(b'\n')
                last = chunk[-1:]
        raw += bool(last and last != b'\n')
        if not 1000 <= raw <= 2147483647:
            raise ValueError(f'{path}: F request count outside [1000, INT_MAX]')
    executed = raw // 1000 * 1000
    tail = raw - executed
    if tail:
        with open(path, 'rb') as stream:
            stream.seek(0, 2)
            position, suffix = stream.tell(), b''
            while position and suffix.count(b'\n') <= tail:
                size = min(position, 65536)
                position -= size
                stream.seek(position)
                suffix = stream.read(size) + suffix
        lines = suffix.splitlines()[-tail:]
        if len(lines) != tail:
            raise ValueError(f'{path}: incomplete F tail')
        for line in lines:
            match = re.fullmatch(rb'(R) user[0-9]+[ \t]*|(U) user[0-9]+[ \t]+\S+[ \t]*', line)
            if not match:
                raise ValueError(f'{path}: invalid F tail operation')
            tail_ops[(match[1] or match[2]).decode()] += 1
    return dict(raw_requests=raw, requests=executed, tail_requests=tail,
                tail_operations=tail_ops)


def measured_result(log, result, test_file, expected, warm_expected, *, tail_operations=None):
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
    tail_operations = tail_operations or {}
    actual_ops = [int(value) - tail_operations.get(op, 0)
                  for op, value in zip(('I', 'U', 'R', 'S'), final[1:5])]
    if any(value < 0 for value in actual_ops) or sum(actual_ops) != expected:
        raise ValueError('operation counters do not match measured requests after tail subtraction')
    if actual_ops[0] or actual_ops[3]:
        raise ValueError('A/F workload must contain only READ and UPDATE operations')
    if not math.isclose(float(final[5]), elapsed, rel_tol=0, abs_tol=1e-6):
        raise ValueError('log and result elapsed times disagree')
    tolerance = max(1e-5, rate * 1e-6 / elapsed)
    if abs(expected / elapsed - rate) > tolerance:
        raise ValueError('reported throughput does not match count and elapsed time')
    return seconds, throughput


FIELDS = ['distribution', 'workload', 'round', 'workers', 'injectors', 'kv_size_bytes',
          'cache_mib', 'raw_requests', 'requests', 'tail_requests', 'seconds', 'throughput_ops_s', 'status', 'error']


def aggregate(summary, destination):
    import statistics
    with open(summary, newline='') as stream:
        rows = list(csv.DictReader(stream))
    fields = ['distribution', 'workload', 'successful_runs', 'mean_ops_s', 'stddev_ops_s',
              'min_ops_s', 'max_ops_s']
    with open(destination, 'w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        groups = dict.fromkeys((row['distribution'], row.get('workload', 'A')) for row in rows)
        for distribution, workload in groups:
            rates = [float(r['throughput_ops_s']) for r in rows
                     if r['distribution'] == distribution and r.get('workload', 'A') == workload
                     and r['status'] == 'ok']
            writer.writerow(dict(zip(fields, [distribution, workload, len(rates),
                statistics.mean(rates) if rates else '',
                statistics.stdev(rates) if len(rates) > 1 else (0 if rates else ''),
                min(rates) if rates else '', max(rates) if rates else ''])))


def main():
    parser = argparse.ArgumentParser(description='Rebuild per-distribution/workload throughput aggregates (sample standard deviation).')
    parser.add_argument('summary', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    destination = args.output or args.summary.with_name('aggregate.csv')
    aggregate(args.summary, destination)
    print(destination.read_text(), end='')


if __name__ == '__main__':
    main()
