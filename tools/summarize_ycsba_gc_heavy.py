#!/usr/bin/env python3
"""Validate GC-heavy phase windows; SSD WA counts host writes, not NAND writes."""
import argparse
import json
import math
from pathlib import Path
import re

PHASES = ('load', 'warmup', 'precondition', 'measured')
BYTE_FIELDS = ('flush_bytes', 'gc_bytes', 'split_bytes', 'merge_bytes')
WINDOW_FIELDS = {'phase', 'requests', 'inserts', 'updates', 'reads', 'scans',
                 'kv_size_bytes', 'seconds', *BYTE_FIELDS}


def inspect_trace(path, phase, expected):
    """Validate exactly the format consumed by the non-production C reader."""
    pattern = re.compile(rb'([IUR]) user([0-9]{1,20})(?:[ \t]+([^\s]+))?[ \t]*(?:\n|\r\n)?')
    counts = dict(inserts=0, updates=0, reads=0, scans=0, requests=0)
    names = {b'I': 'inserts', b'U': 'updates', b'R': 'reads'}
    with Path(path).open('rb') as stream:
        for number, line in enumerate(stream, 1):
            match = pattern.fullmatch(line)
            if not match or int(match[2]) > 18446744073709551615:
                raise ValueError(f'{path}:{number}: invalid trace record')
            op, key, value = match.groups()
            if (phase == 'load' and op != b'I') or (phase != 'load' and op not in (b'R', b'U')):
                raise ValueError(f'{path}:{number}: unexpected operation for {phase}')
            # client_read_request uses a 100-byte token buffer.
            if (op == b'R') != (value is None) or (value is not None and len(value) >= 100):
                raise ValueError(f'{path}:{number}: invalid value token')
            counts[names[op]] += 1
            counts['requests'] += 1
            if number > expected:
                raise ValueError(f'{path}: expected exactly {expected} requests')
    if counts['requests'] != expected or expected <= 0 or expected % 1000:
        raise ValueError(f'{path}: expected exactly {expected} requests, got {counts["requests"]}')
    if phase != 'load' and (not counts['updates'] or not counts['reads']):
        raise ValueError(f'{path}: YCSB A requires both READ and UPDATE')
    return counts


def read_windows(path):
    events, windows, active = [], {}, None
    with Path(path).open() as stream:
        for raw in stream:
            line = raw.rstrip('\n')
            if line.startswith('Benchmark phase: '):
                fields = line.split()
                if len(fields) != 4:
                    raise ValueError('malformed phase marker')
                event, phase = fields[2:]
                events.append((event, phase))
                if event == 'begin' and active is None:
                    active = phase
                elif event == 'end' and active == phase:
                    active = None
                else:
                    raise ValueError('out-of-order phase marker')
            elif line.startswith('SSD window: '):
                window = json.loads(line[len('SSD window: '):])
                if set(window) != WINDOW_FIELDS:
                    raise ValueError('invalid SSD window fields')
                phase = window['phase']
                if phase not in ('precondition', 'measured') or phase != active or phase in windows:
                    raise ValueError('duplicate or misplaced SSD window')
                windows[phase] = window
    if events != [(event, phase) for phase in PHASES for event in ('begin', 'end')]:
        raise ValueError('missing, duplicate or out-of-order benchmark phase')
    if set(windows) != {'precondition', 'measured'}:
        raise ValueError('missing SSD window')
    return windows


def summarize(log, result, inputs, kv_size):
    windows = read_windows(log)
    if windows != read_windows(result):
        raise ValueError('log and result SSD windows disagree')
    for phase, window in windows.items():
        for key in WINDOW_FIELDS - {'phase', 'seconds'}:
            if type(window[key]) is not int or window[key] < 0:
                raise ValueError(f'invalid {key}')
        seconds = window['seconds']
        if type(seconds) not in (int, float) or not math.isfinite(seconds) or seconds <= 0:
            raise ValueError('invalid elapsed seconds')
        for key, expected in inputs[phase].items():
            if window[key] != expected:
                raise ValueError(f'{phase}: {key} mismatch')
        if window['requests'] != sum(window[k] for k in ('inserts', 'updates', 'reads', 'scans')):
            raise ValueError('operation counts do not add up')
        if window['inserts'] or window['scans'] or not window['updates'] or not window['reads']:
            raise ValueError('expected YCSB A READ/UPDATE operations')
        if window['kv_size_bytes'] != kv_size:
            raise ValueError('KV size mismatch')
        if any(window[key] % 4096 for key in BYTE_FIELDS):
            raise ValueError('SSD bytes are not whole 4096-byte blocks')
    final = windows['measured']
    logical = final['updates'] * kv_size
    written = sum(final[key] for key in BYTE_FIELDS)
    return dict(final, logical_update_bytes=logical, ssd_write_bytes=written,
                throughput_ops_s=final['requests'] / final['seconds'],
                throughput_mops_s=final['requests'] / final['seconds'] / 1e6,
                ssd_write_amplification=written / logical,
                gc_observed=bool(final['gc_bytes']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result_dir', type=Path)
    args = parser.parse_args()
    c = json.loads((args.result_dir / 'config.json').read_text())
    status = json.loads((args.result_dir / 'status.json').read_text())
    if status['status'] != 'ok':
        raise ValueError('experiment did not complete successfully')
    print(json.dumps(summarize(args.result_dir / 'uniform_A.log',
        args.result_dir / 'uniform_A.result.txt', c['INPUTS'], c['KV_SIZE']), indent=2))


if __name__ == '__main__':
    main()
