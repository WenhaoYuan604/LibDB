#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
Run the LibDB 1 KiB heavy test in three phases:
  1. fresh database + YCSB A
  2. fresh database + YCSB C
  3. recover the database left by phase 2 + YCSB C verification

Usage:
  sudo ./run_heavy_ac_recovery.sh

Defaults:
  workers=16, load injectors=8, cache=20000 MiB, KV size=1024 bytes
  KV count=100000000, tasks per worker=16, boundary size=1000

Required environment:
  LIBDB_ALLOW_DESTRUCTIVE=1  Acknowledge that the experiment SSD is disposable.
  PCI_ADDR=DDDD:BB:DD.F      Dedicated experiment NVMe controller PCI BDF.
  PMEM_BASE=/mnt/pmem0/libdb Parent directory for heavy-test PMEM files.

Optional environment overrides:
  WORKLOAD_DIR=<repo>/workload
  WARM_FILE=$WORKLOAD_DIR/warm.txt
  YCSBA_FILE=$WORKLOAD_DIR/ycsba.txt
  YCSBC_FILE=$WORKLOAD_DIR/ycsbc.txt
  PMEM_DIR=$PMEM_BASE/libdb_clean_heavy_ac
  RESULT_BASE=<repo>/results
  BINARY=<repo>/build/test
  RECOVERY_DUPCHECK=1
  TIMEOUT_SECONDS=0       0 disables the per-phase timeout
  KILL_AFTER_SECONDS=30
  USE_SUDO=1

Only PMEM_DIR is deleted before fresh phases and when the script exits. The
workload directory and its workload files are never modified.
The NVMe device named by PCI_ADDR is treated as a disposable experiment disk.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

require_positive_integer() {
    local name=$1
    local value=$2
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must be a positive integer (got '$value')"
}

require_nonnegative_integer() {
    local name=$1
    local value=$2
    [[ "$value" =~ ^[0-9]+$ ]] || die "$name must be a non-negative integer (got '$value')"
}

require_flag() {
    local name=$1
    local value=$2
    [[ "$value" == "0" || "$value" == "1" ]] || die "$name must be 0 or 1 (got '$value')"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
[[ $# -eq 0 ]] || die "unknown argument '$1'; use --help"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BINARY=${BINARY:-"${SCRIPT_DIR}/build/test"}
PCI_ADDR=${PCI_ADDR:-}
WORKLOAD_DIR=${WORKLOAD_DIR:-"${SCRIPT_DIR}/workload"}
WARM_FILE=${WARM_FILE:-"${WORKLOAD_DIR}/warm.txt"}
YCSBA_FILE=${YCSBA_FILE:-"${WORKLOAD_DIR}/ycsba.txt"}
YCSBC_FILE=${YCSBC_FILE:-"${WORKLOAD_DIR}/ycsbc.txt"}
PMEM_BASE=${PMEM_BASE:-}
PMEM_DIR=${PMEM_DIR:-${PMEM_BASE:+${PMEM_BASE}/libdb_clean_heavy_ac}}
RESULT_BASE=${RESULT_BASE:-"${SCRIPT_DIR}/results"}

THREADS=${THREADS:-16}
KV_SIZE=${KV_SIZE:-1024}
KV_COUNT=${KV_COUNT:-100000000}
CACHE_SIZE_MB=${CACHE_SIZE_MB:-20000}
LOAD_INJECTORS=${LOAD_INJECTORS:-8}
TASKS_PER_THREAD=${TASKS_PER_THREAD:-16}
BOUNDARY_SIZE=${BOUNDARY_SIZE:-1000}
RECOVERY_DUPCHECK=${RECOVERY_DUPCHECK:-1}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-0}
KILL_AFTER_SECONDS=${KILL_AFTER_SECONDS:-30}
USE_SUDO=${USE_SUDO:-1}
SKIP_DRIVER_CHECK=${SKIP_DRIVER_CHECK:-0}
LIBDB_ALLOW_DESTRUCTIVE=${LIBDB_ALLOW_DESTRUCTIVE:-0}

require_positive_integer THREADS "$THREADS"
require_positive_integer KV_SIZE "$KV_SIZE"
require_positive_integer KV_COUNT "$KV_COUNT"
require_nonnegative_integer CACHE_SIZE_MB "$CACHE_SIZE_MB"
require_positive_integer LOAD_INJECTORS "$LOAD_INJECTORS"
require_positive_integer TASKS_PER_THREAD "$TASKS_PER_THREAD"
require_positive_integer BOUNDARY_SIZE "$BOUNDARY_SIZE"
require_flag RECOVERY_DUPCHECK "$RECOVERY_DUPCHECK"
require_nonnegative_integer TIMEOUT_SECONDS "$TIMEOUT_SECONDS"
require_positive_integer KILL_AFTER_SECONDS "$KILL_AFTER_SECONDS"
require_flag USE_SUDO "$USE_SUDO"
require_flag SKIP_DRIVER_CHECK "$SKIP_DRIVER_CHECK"
[[ "$LIBDB_ALLOW_DESTRUCTIVE" == "1" ]] || die \
    "set LIBDB_ALLOW_DESTRUCTIVE=1 after verifying PCI_ADDR is a disposable experiment SSD"
[[ -n "$PCI_ADDR" ]] || die "set PCI_ADDR to the dedicated experiment NVMe controller"
[[ -n "$PMEM_BASE" ]] || die "set PMEM_BASE to the parent directory for heavy-test PMEM files"

command -v realpath >/dev/null 2>&1 || die "realpath is required"
command -v tee >/dev/null 2>&1 || die "tee is required"
if [[ "$TIMEOUT_SECONDS" -gt 0 ]]; then
    command -v timeout >/dev/null 2>&1 || die "timeout is required when TIMEOUT_SECONDS is non-zero"
fi

[[ -x "$BINARY" ]] || die "LibDB executable not found: $BINARY"
[[ -f "$WARM_FILE" ]] || die "warm workload not found: $WARM_FILE"
[[ -f "$YCSBA_FILE" ]] || die "YCSB A workload not found: $YCSBA_FILE"
[[ -f "$YCSBC_FILE" ]] || die "YCSB C workload not found: $YCSBC_FILE"

BINARY=$(realpath "$BINARY")
WARM_FILE=$(realpath "$WARM_FILE")
YCSBA_FILE=$(realpath "$YCSBA_FILE")
YCSBC_FILE=$(realpath "$YCSBC_FILE")
PMEM_BASE=$(realpath -m -- "$PMEM_BASE")
PMEM_DIR=$(realpath -m -- "$PMEM_DIR")
RESULT_BASE=$(realpath -m -- "$RESULT_BASE")

[[ "$PMEM_BASE" != "/" ]] || die "refusing to use / as PMEM_BASE"
case "$PMEM_DIR" in
    "${PMEM_BASE}"/*) ;;
    *) die "PMEM_DIR must be a child of PMEM_BASE ($PMEM_BASE): $PMEM_DIR" ;;
esac
[[ "$PMEM_DIR" != "$PMEM_BASE" ]] || die "refusing to remove PMEM_BASE itself"

root_command=()
if [[ "$USE_SUDO" == "1" && "$EUID" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "sudo is required when USE_SUDO=1"
    sudo -v
    root_command=(sudo)
fi

if [[ ! -d "$PMEM_BASE" ]]; then
    printf 'Creating PMEM base directory: %s\n' "$PMEM_BASE"
    "${root_command[@]}" mkdir -p -- "$PMEM_BASE"
fi
[[ -d "$PMEM_BASE" ]] || die "failed to create PMEM_BASE: $PMEM_BASE"

if [[ "$SKIP_DRIVER_CHECK" == "0" ]]; then
    sysfs_device="/sys/bus/pci/devices/${PCI_ADDR}"
    [[ -d "$sysfs_device" ]] || die "PCI device does not exist: $PCI_ADDR"
    [[ -L "${sysfs_device}/driver" ]] || die "PCI device $PCI_ADDR has no bound driver"
    pci_driver=$(basename "$(readlink -f "${sysfs_device}/driver")")
    case "$pci_driver" in
        uio_pci_generic|vfio-pci) ;;
        *) die "PCI device $PCI_ADDR uses '$pci_driver', expected uio_pci_generic or vfio-pci" ;;
    esac
fi

run_id=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${RESULT_BASE}/heavy_ac_${run_id}"
mkdir -p -- "$RESULT_DIR"

cleanup_pmem() {
    printf 'Cleaning heavy-test PMEM directory: %s\n' "$PMEM_DIR"
    "${root_command[@]}" rm -rf -- "$PMEM_DIR"
}

finish() {
    local status=$?
    trap - EXIT INT TERM
    if ! cleanup_pmem; then
        printf 'warning: failed to clean PMEM directory: %s\n' "$PMEM_DIR" >&2
        [[ $status -ne 0 ]] || status=1
    fi
    if [[ $status -eq 0 ]]; then
        printf 'All heavy-test phases completed successfully.\n'
        printf 'Results and logs: %s\n' "$RESULT_DIR"
    else
        printf 'Heavy test stopped with status %d. Logs retained in: %s\n' "$status" "$RESULT_DIR" >&2
    fi
    exit "$status"
}

trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

prepare_fresh_database() {
    cleanup_pmem
    "${root_command[@]}" mkdir -p -- "$PMEM_DIR"
}

run_phase() {
    local label=$1
    local test_file=$2
    local recovery=$3
    local output_file="${RESULT_DIR}/${label}.result.txt"
    local log_file="${RESULT_DIR}/${label}.log"
    local status mode
    local -a arguments command

    arguments=(
        "--thread=${THREADS}"
        "--kvsize=${KV_SIZE}"
        "--kvs=${KV_COUNT}"
        "--cachesize=${CACHE_SIZE_MB}"
        "--load-injectors=${LOAD_INJECTORS}"
        "--tasks-per-thread=${TASKS_PER_THREAD}"
        "--boundary-size=${BOUNDARY_SIZE}"
        "--pci-addr=${PCI_ADDR}"
        "--pmem-dir=${PMEM_DIR}"
        "--warm=${WARM_FILE}"
        "--test=${test_file}"
        "--output=${output_file}"
    )
    if [[ "$recovery" == "1" ]]; then
        arguments+=(--recovery "--recovery-dupcheck=${RECOVERY_DUPCHECK}")
        mode=recovery
    else
        mode=fresh
    fi

    command=("${root_command[@]}")
    if [[ "$TIMEOUT_SECONDS" -gt 0 ]]; then
        command+=(timeout --signal=INT --kill-after="${KILL_AFTER_SECONDS}s" "${TIMEOUT_SECONDS}s")
    fi
    command+=("$BINARY" "${arguments[@]}")

    printf '\n============================================================\n'
    printf 'Starting phase: %s\n' "$label"
    printf 'Mode: %s\n' "$mode"
    printf 'Workload: %s\n' "$test_file"
    printf 'Output: %s\n' "$output_file"
    printf 'Log: %s\n' "$log_file"
    printf '============================================================\n'

    set +e
    "${command[@]}" 2>&1 | tee "$log_file"
    status=${PIPESTATUS[0]}
    set -e

    if [[ $status -eq 0 ]]; then
        printf 'Phase %s completed successfully.\n' "$label"
    elif [[ $status -eq 124 ]]; then
        printf 'Phase %s reached the %ss timeout.\n' "$label" "$TIMEOUT_SECONDS" >&2
    else
        printf 'Phase %s failed with status %d.\n' "$label" "$status" >&2
    fi
    return "$status"
}

printf 'LibDB heavy A/C + recovery test\n'
printf 'PCI device: %s\n' "$PCI_ADDR"
printf 'workers=%s, injectors=%s, cache=%s MiB, KV size=%s bytes, KVs=%s\n' \
    "$THREADS" "$LOAD_INJECTORS" "$CACHE_SIZE_MB" "$KV_SIZE" "$KV_COUNT"
printf 'PMEM directory: %s\n' "$PMEM_DIR"
printf 'Results directory: %s\n' "$RESULT_DIR"

# YCSB A and C are independent benchmark runs, so both start from a fresh DB.
prepare_fresh_database
run_phase ycsba "$YCSBA_FILE" 0

prepare_fresh_database
run_phase ycsbc "$YCSBC_FILE" 0

# Do not clean after C: recovery must consume exactly C's PMEM/NVMe state.
run_phase recovery_ycsbc "$YCSBC_FILE" 1
