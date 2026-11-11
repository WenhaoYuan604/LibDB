# LibDB

LibDB is a research key-value storage prototype that combines DRAM, persistent
memory (PMEM), and NVMe SSDs. It uses PMDK for persistent-memory management and
SPDK for asynchronous, user-space NVMe I/O. This repository contains the database
implementation, a single run example, and YCSB trace-generation materials.

## Design

- Partitioned key ranges with per-worker indexes and asynchronous I/O tasks.
- Persistent write buffers and indexes, together with a DRAM key-value cache.
- Group-based flushing, splitting, merging, and SSD garbage collection.
- Read, write, delete, and range-scan paths, plus index reconstruction on recovery.
- Built-in throughput, latency, storage utilization, and SSD write statistics.

This is an experimental implementation.

## Requirements

- Linux, a C/C++ toolchain, CMake 3.13+, `pkg-config`, and Bash.
- A built SPDK source tree, including DPDK and the `spdk_nvme`, `spdk_env_dpdk`,
  and `spdk_syslibs` pkg-config metadata.
- PMDK development headers/libraries (`libpmem` and `libpmemobj`), `libnuma`,
  and UUID development files.
- For hardware experiments: a disposable NVMe SSD with 4 KiB logical blocks,
  a writable PMEM/DAX filesystem, and sufficient DRAM and SPDK hugepage memory.
- For trace generation: Git, Java, and Maven. Generated traces and temporary
  databases require substantial disk space at the default 100M-record scale.

The build has been checked on Ubuntu 20.04 with GCC 9.4 and CMake 3.24, using a
local SPDK tree based on commit
`7be0593aa3c5aa5748ec2a2feb507b4db75bf35b` (`v23.09-pre-131-g7be0593aa`).
Other SPDK revisions have not been validated; the build directly includes
`lib/nvme/nvme_pcie.c` from the supplied SPDK source tree.

## Build

Prepare the dependencies above, then run. Commands below use the database
subdirectory as the working directory:

```bash
git clone https://github.com/WenhaoYuan604/LibDB.git
cd LibDB/LibDB
export SPDK_ROOT=/path/to/built/spdk
cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT"
cmake --build build --target libdb --parallel 6
```

The main CMake target is `libdb`; its executable is `build/test`.
Run `./build/test --help` for runtime options.

## Generate workloads

The bundled [YCSB generator](../artifact/ycsb/README.md) produces `warm.txt`,
`ycsba.txt`, `ycsbc.txt`, and `ycsbe.txt`. For a small generator check:

```bash
YCSB_ALLOW_DESTRUCTIVE=1 \
YCSB_DB_DIR=/path/to/new/disposable/ycsb-database \
RECORD_COUNT=1000 \
YCSBA_OPERATION_COUNT=1000 \
YCSBC_OPERATION_COUNT=1000 \
YCSBE_OPERATION_COUNT=1000 \
bash ./../artifact/ycsb/generate_workloads.sh
```

`YCSB_DB_DIR` must not already exist: the generator creates and removes this
temporary RocksDB database. Traces are written to `../artifact/ycsb/generated/`.
For full-size traces, omit the four count overrides: defaults are 100M load,
100M A, 100M C, and 20M E requests. You can also supply existing compatible
text traces. The generator documentation describes the inputs and output format.

## Run an experiment

> **Destructive experiment:** LibDB writes directly to the selected NVMe SSD.
> Use only a dedicated device whose entire contents may be discarded, and a
> dedicated PMEM directory. Never use a system disk or data you need to retain.

Configure SPDK hugepages and bind the dedicated controller to an SPDK-compatible
driver using your SPDK installation's setup procedure. Verify its PCI address
and unmount its filesystems first. `runexample.sh` does not bind devices by
default; `PREPARE_SPDK=1` explicitly enables SPDK setup.

After generating the small traces above, replace the device and directory
placeholders and run:

```bash
LIBDB_ALLOW_DESTRUCTIVE=1 \
SPDK_ROOT=/path/to/built/spdk \
PCI_ADDR=DDDD:BB:DD.F \
PMEM_DIR=/path/to/existing/dedicated/pmem-directory \
WORKLOAD_DIR="$PWD/../artifact/ycsb/generated" \
KV_COUNT=1000 \
bash ./runexample.sh
```

The script builds LibDB by default and runs one load/test sequence. `WORKLOAD_DIR`
selects `warm.txt` for loading and `ycsbc.txt` for testing. `KV_COUNT` must match
the intended load size; set it to `100000000` for the generator's default
full-size load. Reducing the trace size does not reduce all memory budgets:
worker count, cache size, and persistent-buffer capacity have separate settings.

| Setting | Purpose / default |
| --- | --- |
| `WARM_FILE`, `TEST_FILE` | Override the load or test trace individually |
| `KV_SIZE` | Logical record size in bytes; default `1024` |
| `THREADS`, `LOAD_INJECTORS` | Worker/injector counts; defaults `16` / `8` |
| `CACHE_SIZE_MB` | Cache capacity in MiB; default `20000` |
| `BUILD`, `BUILD_DIR` | Build before running (`1`); binary location defaults to `build/test` |
| `RESULT_DIR` | Output directory; default `results/` |
| `RECOVERY`, `RECOVERY_DUPCHECK` | Recovery and optional duplicate-key check; defaults `0` / `0` |
| `USE_SUDO` | Invoke LibDB through sudo when needed; default `1` |

To run A or E instead of C, add `TEST_FILE="$PWD/../artifact/ycsb/generated/ycsba.txt"`
or `TEST_FILE="$PWD/../artifact/ycsb/generated/ycsbe.txt"` to the same invocation.
For an independent fresh experiment, prepare fresh disposable database storage.
For recovery, keep the previous database's SSD contents and `PMEM_DIR`, use
compatible build/runtime settings, and add `RECOVERY=1` (optionally
`RECOVERY_DUPCHECK=1`). Both trace paths are still required by the script.

`runexample.sh` does not generate traces, format or mount storage, clean PMEM
files, or execute an experiment matrix. `PMEM_DIR` must already exist. Results
and combined terminal logs are written to `result_<timestamp>.txt` and
`run_<timestamp>.log` under `RESULT_DIR`. Run `bash ./runexample.sh --help` for
all parameters, including task concurrency and build options.
