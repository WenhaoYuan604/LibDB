# LibDB-DRAM

LibDB-DRAM is a research key-value store with DRAM write buffers and indexes,
SPDK NVMe I/O, and an optional per-worker SSD write-ahead log (WAL). It uses
key-range groups, asynchronous reads and scans, and group-based flushing,
splitting, merging, and garbage collection. The example below enables WAL.

The implementation includes throughput, latency, SSD space, and per-phase I/O
statistics. `wa_data` counts data writes, `wa_wal` counts WAL writes, and
`wa_total` includes both, divided by the logical record bytes written by the
submitted INSERT/UPDATE requests. Read-only phases report `N/A`. Phase I/O is
sampled without forcing a flush or draining background work at the boundary.

## Build

Requirements: Linux, a C/C++ toolchain, CMake 3.13+, pkg-config, Bash, libnuma,
UUID development files, and a built SPDK tree with its DPDK backend. SPDK must
provide `spdk_nvme`, `spdk_env_dpdk`, and `spdk_syslibs` pkg-config metadata and
`rte_launch.h`. Unlike the PMEM variant, this target does not require PMDK or a
PMEM filesystem. The build directly includes SPDK's `lib/nvme/nvme_pcie.c`.

```bash
git clone https://github.com/WenhaoYuan604/LibDB.git
cd LibDB/LibDB-DRAM
export SPDK_ROOT=/path/to/built/spdk
cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT"
cmake --build build --target libdb --parallel 6
./build/test --help
```

The executable is `build/test`. The compiler settings and runtime options must
match any existing database being recovered.

## Generate workloads

Use the shared [YCSB generator](../artifact/ycsb/README.md), which requires Git,
Java, and Maven. For a small trace set:

```bash
YCSB_ALLOW_DESTRUCTIVE=1 \
YCSB_DB_DIR=/path/to/new/disposable/ycsb-database \
RECORD_COUNT=1000 \
YCSBA_OPERATION_COUNT=1000 \
YCSBC_OPERATION_COUNT=1000 \
YCSBE_OPERATION_COUNT=1000 \
bash ../artifact/ycsb/generate_workloads.sh
```

The generator creates and removes its temporary RocksDB database; its directory
must not already exist. Traces are written to `../artifact/ycsb/generated/`.
Existing compatible traces can also be supplied. Small traces do not reduce
fixed buffer, cache, WAL, or hugepage budgets automatically.

## Run

> **Destructive experiment:** the selected SSD is written directly. Use a
> disposable NVMe controller with one active namespace and 4 KiB logical blocks.
> Keep traces, binaries, and logs on separate storage. Do not select a system
> disk or a device containing data you need.

Prepare hugepages, unmount the experiment SSD, and bind it to an SPDK-compatible
driver. Driver binding is opt-in through `PREPARE_SPDK=1`; it is disabled by
default. The script requires `findmnt`, `realpath`, `readlink`, `mktemp`, and
`tee`, and checks workload/output filesystem isolation from the selected SSD.

```bash
LIBDB_ALLOW_DESTRUCTIVE=1 \
SPDK_ROOT=/path/to/built/spdk \
PCI_ADDR=DDDD:BB:DD.F \
WORKLOAD_DIR="$PWD/../artifact/ycsb/generated" \
KV_COUNT=1000 \
bash ./runexample.sh
```

The default sequence loads `warm.txt`, warms the cache, and measures `ycsbc.txt`.
Set `TEST_FILE` to select A or E. Set `PRECONDITION_A_FILE` to an A trace to run
it after cache warmup and before the measured trace:

```bash
LIBDB_ALLOW_DESTRUCTIVE=1 \
SPDK_ROOT=/path/to/built/spdk \
PCI_ADDR=DDDD:BB:DD.F \
WORKLOAD_DIR="$PWD/../artifact/ycsb/generated" \
KV_COUNT=1000 \
TEST_FILE="$PWD/../artifact/ycsb/generated/ycsbe.txt" \
PRECONDITION_A_FILE="$PWD/../artifact/ycsb/generated/ycsba.txt" \
bash ./runexample.sh
```

For full-size traces, set `KV_COUNT` to the intended load size.

| Setting | Meaning / default |
| --- | --- |
| `WARM_FILE`, `TEST_FILE` | Individual trace overrides |
| `THREADS`, `LOAD_INJECTORS`, `TASKS_PER_THREAD` | `16`, `8`, `16` |
| `KV_SIZE`, `KV_COUNT` | Record bytes `1024`; load count `100000000` |
| `CACHE_SIZE_MB` | DRAM cache: `16384` MiB |
| `WAL` | Enable PUT WAL: `1` |
| `WAL_SIZE_MB_PER_WORKER` | `8192` MiB per worker; 128 GiB total at 16 workers |
| `WAL_MAX_INFLIGHT`, `WAL_WAIT_US` | `1` batch; `10` microseconds |
| `PHASE_IO_STATS` | Phase I/O and write amplification: `1` |
| `RECOVERY`, `RECOVERY_SCAN` | `0`; `first-empty` or `full` |
| `BUILD`, `BUILD_DIR` | `1`; `./build` |
| `RESULT_DIR`, `USE_SUDO` | `./results`; `1` |

The source defaults to a shared 16 GiB DRAM write-buffer budget. WAL reserves
SSD capacity in addition to the data area. `WAL=0` is available for experiments,
but recovery requires `WAL=1`. The example's WAL, cache and phase-statistics
defaults are supplied by the script; direct binary invocations must set them.

For recovery, retain the SSD contents, use matching worker count, layout and WAL
capacity, and add `RECOVERY=1`. `RECOVERY_SCAN=first-empty` assumes occupied
physical segments are contiguous and stops at the first all-zero segment;
`RECOVERY_SCAN=full` scans the full data region. Trace paths remain required.
The legacy binary option `--recovery-dupcheck` no longer performs a duplicate-key
check and is not exposed by this script.

`runexample.sh` does not generate traces, clear storage, scan input contents,
access PMEM, tune CPU/NUMA, or run an experiment matrix. Fresh runs require
appropriately prepared disposable storage. Results and combined stdout/stderr
are saved with unique names under `RESULT_DIR`. Use `--help` for all options.

## Limitations and licensing

This is an experimental implementation. WAL supports PUT records; DELETE is
unsupported with WAL and is not recoverable. Recovery requires the current WAL
format and compatible layout. The format cannot reliably identify every stale
record or torn write. Recovery of GC-migrated records that have lost their
persistent copy remains a limitation; full scanning does not remove it.
Flush padding is cleared before reuse, but existing SSD contents are not repaired
automatically. Recovery tests do not establish power-loss safety for all paths.

No project license has been specified. The shared YCSB material retains its
[license](../artifact/ycsb/LICENSE-YCSB.txt) and
[third-party notice](../artifact/ycsb/NOTICE.txt).
