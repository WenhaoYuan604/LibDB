# LibDB YCSB A/C/E workload generator

This directory reproduces the text traces consumed by LibDB without storing
multi-gigabyte generated files in Git. It uses
[`haslaboratory/YCSB-tracegen`](https://github.com/haslaboratory/YCSB-tracegen)
at the pinned commit below:

```text
314c6490956e8deb9f9ee71ee2bd48d1bfd2e510
```

The upstream trace generator already logs insert, read, and update operations.
`patches/0001-log-scan-operations.patch` adds the missing `S key length` record
for YCSB E. The upstream YCSB code and the derived files in this directory are
covered by Apache-2.0; see `LICENSE-YCSB.txt` and `NOTICE.txt`.

## Generated traces

| File | Phase | Default operations | Profile |
| --- | --- | ---: | --- |
| `warm.txt` | YCSB load | 100,000,000 | inserts |
| `ycsba.txt` | YCSB A | 100,000,000 | 50% read, 50% update; Zipfian |
| `ycsbc.txt` | YCSB C | 100,000,000 | 100% read; uniform |
| `ycsbe.txt` | YCSB E | 20,000,000 | 95% scan, 5% insert; Zipfian start keys; uniform scan length 1--100 |

The E parameters match the retained trace used by the LibDB experiments: it
contains 20,000,000 operations, and its 18,998,995 scans have minimum length 1,
maximum length 100, and mean length 50.498947.

YCSB uses concurrent generator threads and random generators, so byte-for-byte
output and operation ordering are not expected to be stable across runs. The
script writes `SHA256SUMS` for transfer integrity and `MANIFEST.txt` for the
parameters and line counts of each generated set.

## Requirements

- Bash, Git, Java, Maven, `sha256sum`, and `realpath`
- Network access to clone the pinned public repository, unless
  `YCSB_REPO_URL` points to a local clone
- A new, disposable RocksDB directory with enough capacity
- Approximately 10 GB for the default text traces and more than 100 GB for the
  temporary 100M-record RocksDB database

The generator first uses the public HTTPS URL. If this server's Git lacks the
HTTPS transport, it automatically retries the same repository through GitHub
SSH; `YCSB_REPO_URL` and `YCSB_SSH_REPO_URL` can both be overridden.

## Generate the paper-scale workloads

Choose a path that does not exist and is safe for the script to create and
remove:

```bash
YCSB_ALLOW_DESTRUCTIVE=1 \
YCSB_DB_DIR=/mnt/experiment/libdb-ycsb-generator \
./artifact/ycsb/generate_workloads.sh
```

Outputs are written to `artifact/ycsb/generated/` by default. The script
refuses an existing RocksDB directory and refuses to overwrite generated files
unless `OVERWRITE_OUTPUT=1` is explicitly set. It removes only the RocksDB
directory it created; set `KEEP_DB=1` to retain it.

## Quick generator check

The following keeps the same operation mixes while reducing every phase to
1,000 operations/records:

```bash
YCSB_ALLOW_DESTRUCTIVE=1 \
YCSB_DB_DIR=/tmp/libdb-ycsb-generator-smoke \
OUTPUT_DIR=/tmp/libdb-ycsb-generated-smoke \
RECORD_COUNT=1000 \
YCSBA_OPERATION_COUNT=1000 \
YCSBC_OPERATION_COUNT=1000 \
YCSBE_OPERATION_COUNT=1000 \
./artifact/ycsb/generate_workloads.sh
```

Use `./artifact/ycsb/generate_workloads.sh --help` for all overrides.
