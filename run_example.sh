#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
Run one LibDB workload safely from an external workload directory.

Usage:
  ./run_example.sh [--help]

Required environment:
  LIBDB_ALLOW_DESTRUCTIVE=1  Acknowledge that LibDB may overwrite experiment devices.
  WORKLOAD_DIR=/path         Directory containing warm.txt and ycsbc.txt.
  PCI_ADDR=0000:5c:00.0      Dedicated experiment NVMe controller PCI BDF.
  PMEM_DIR=/mnt/pmem0        Existing writable PMEM/DAX directory.

  Alternatively set WARM_FILE and TEST_FILE directly.

Common options:
  SPDK_ROOT=../spdk          SPDK source tree.
  THREADS=16                 Number of server workers.
  KV_SIZE=1024               KV size in bytes.
  KV_COUNT=100000000         KV count used for queue sizing/configuration.
  CACHE_SIZE_MB=20000        KV cache size in MiB.
  LOAD_INJECTORS=8           Client injector count.
  TASKS_PER_THREAD=16        Outstanding tasks per server worker.
  BOUNDARY_SIZE=1000         Initial boundary sample size.
  BUILD=1                    Configure and build before running (0 or 1).
  BUILD_JOBS=<nproc>         Parallel build jobs.
  USE_SUDO=1                 Run SPDK setup and LibDB through sudo (0 or 1).
  PREPARE_SPDK=0             Invoke SPDK scripts/setup.sh before running (0 or 1).
  RECOVERY=0                 Run in recovery mode (0 or 1).
  RECOVERY_DUPCHECK=0        Check all recovered keys for duplicates (0 or 1).
  BUILD_DIR=./build          CMake build directory.
  RESULT_DIR=./results       Output and log directory.

The script never formats or mounts persistent memory. PMEM_DIR must already be
a writable DAX/PMEM mount, and PCI_ADDR must identify a dedicated 4 KiB-sector SSD.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

require_flag() {
    local name=$1
    local value=$2
    [[ "$value" == "0" || "$value" == "1" ]] || die "$name must be 0 or 1 (got '$value')"
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

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
[[ $# -eq 0 ]] || die "unknown argument '$1'; use --help"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SPDK_ROOT=${SPDK_ROOT:-"${SCRIPT_DIR}/../spdk"}
WORKLOAD_DIR=${WORKLOAD_DIR:-}
WARM_FILE=${WARM_FILE:-${WORKLOAD_DIR:+${WORKLOAD_DIR}/warm.txt}}
TEST_FILE=${TEST_FILE:-${WORKLOAD_DIR:+${WORKLOAD_DIR}/ycsbc.txt}}

THREADS=${THREADS:-16}
KV_SIZE=${KV_SIZE:-1024}
KV_COUNT=${KV_COUNT:-100000000}
CACHE_SIZE_MB=${CACHE_SIZE_MB:-20000}
LOAD_INJECTORS=${LOAD_INJECTORS:-8}
TASKS_PER_THREAD=${TASKS_PER_THREAD:-16}
BOUNDARY_SIZE=${BOUNDARY_SIZE:-1000}
BUILD=${BUILD:-1}
BUILD_JOBS=${BUILD_JOBS:-$(nproc)}
USE_SUDO=${USE_SUDO:-1}
PREPARE_SPDK=${PREPARE_SPDK:-0}
PCI_ADDR=${PCI_ADDR:-}
PMEM_DIR=${PMEM_DIR:-}
RECOVERY=${RECOVERY:-0}
RECOVERY_DUPCHECK=${RECOVERY_DUPCHECK:-0}
BUILD_DIR=${BUILD_DIR:-"${SCRIPT_DIR}/build"}
RESULT_DIR=${RESULT_DIR:-"${SCRIPT_DIR}/results"}
LIBDB_ALLOW_DESTRUCTIVE=${LIBDB_ALLOW_DESTRUCTIVE:-0}

require_positive_integer THREADS "$THREADS"
require_positive_integer KV_SIZE "$KV_SIZE"
[[ "$KV_SIZE" -ge 52 ]] || die "KV_SIZE must be at least 52 bytes"
require_positive_integer KV_COUNT "$KV_COUNT"
require_nonnegative_integer CACHE_SIZE_MB "$CACHE_SIZE_MB"
require_positive_integer LOAD_INJECTORS "$LOAD_INJECTORS"
require_positive_integer TASKS_PER_THREAD "$TASKS_PER_THREAD"
require_positive_integer BOUNDARY_SIZE "$BOUNDARY_SIZE"
require_positive_integer BUILD_JOBS "$BUILD_JOBS"
require_flag BUILD "$BUILD"
require_flag USE_SUDO "$USE_SUDO"
require_flag PREPARE_SPDK "$PREPARE_SPDK"
require_flag RECOVERY "$RECOVERY"
require_flag RECOVERY_DUPCHECK "$RECOVERY_DUPCHECK"
if [[ "$RECOVERY_DUPCHECK" == "1" && "$RECOVERY" != "1" ]]; then
    die "RECOVERY_DUPCHECK=1 requires RECOVERY=1"
fi

[[ "$LIBDB_ALLOW_DESTRUCTIVE" == "1" ]] || die \
    "set LIBDB_ALLOW_DESTRUCTIVE=1 after verifying that SPDK only owns a disposable experiment NVMe device"

command -v realpath >/dev/null 2>&1 || die "realpath is required"
command -v tee >/dev/null 2>&1 || die "tee is required"

[[ -n "$WARM_FILE" ]] || die "set WORKLOAD_DIR or WARM_FILE"
[[ -n "$TEST_FILE" ]] || die "set WORKLOAD_DIR or TEST_FILE"
[[ -n "$PCI_ADDR" ]] || die "set PCI_ADDR to the dedicated experiment NVMe controller"
[[ -n "$PMEM_DIR" ]] || die "set PMEM_DIR to an existing PMEM/DAX directory"
[[ -f "$WARM_FILE" ]] || die "warm workload not found: $WARM_FILE"
[[ -f "$TEST_FILE" ]] || die "test workload not found: $TEST_FILE"
[[ -d "$PMEM_DIR" ]] || die "PMEM directory not found: $PMEM_DIR"
WARM_FILE=$(realpath "$WARM_FILE")
TEST_FILE=$(realpath "$TEST_FILE")
PMEM_DIR=$(realpath "$PMEM_DIR")

root_command=()
if [[ "$USE_SUDO" == "1" && "$EUID" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "USE_SUDO=1 but sudo is not installed"
    root_command=(sudo)
fi

if [[ "$BUILD" == "1" || "$PREPARE_SPDK" == "1" ]]; then
    [[ -d "$SPDK_ROOT" ]] || die "SPDK_ROOT is not a directory: $SPDK_ROOT"
    SPDK_ROOT=$(realpath "$SPDK_ROOT")
fi

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="${SCRIPT_DIR}/${BUILD_DIR#./}" ;;
esac
case "$RESULT_DIR" in
    /*) ;;
    *) RESULT_DIR="${SCRIPT_DIR}/${RESULT_DIR#./}" ;;
esac

if [[ "$BUILD" == "1" ]]; then
    command -v cmake >/dev/null 2>&1 || die "cmake is required when BUILD=1"
    export PKG_CONFIG_PATH="${SPDK_ROOT}/build/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DSPDK_ROOT="$SPDK_ROOT"
    cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
fi

BINARY="${BUILD_DIR}/test"
[[ -x "$BINARY" ]] || die "LibDB executable not found: $BINARY (set BUILD=1 or BUILD_DIR correctly)"

if [[ "$PREPARE_SPDK" == "1" ]]; then
    [[ -x "${SPDK_ROOT}/scripts/setup.sh" ]] || die "SPDK setup script not found under $SPDK_ROOT"
    printf 'Binding PCI device %s to SPDK (verify this is the disposable experiment SSD).\n' "$PCI_ADDR"
    "${root_command[@]}" env HUGE_EVEN_ALLOC=yes PCI_ALLOWED="$PCI_ADDR" "${SPDK_ROOT}/scripts/setup.sh"
fi

mkdir -p "$RESULT_DIR"
RESULT_DIR=$(cd "$RESULT_DIR" && pwd)
timestamp=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="${RESULT_DIR}/result_${timestamp}.txt"
LOG_FILE="${RESULT_DIR}/run_${timestamp}.log"
touch "$OUTPUT_FILE"

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
    "--test=${TEST_FILE}"
    "--output=${OUTPUT_FILE}"
)
if [[ "$RECOVERY" == "1" ]]; then
    arguments+=(--recovery)
fi
if [[ "$RECOVERY_DUPCHECK" == "1" ]]; then
    arguments+=("--recovery-dupcheck=1")
fi

printf 'Starting LibDB with warm workload: %s\n' "$WARM_FILE"
printf 'Test workload: %s\n' "$TEST_FILE"
printf 'Output: %s\n' "$OUTPUT_FILE"
printf 'Log: %s\n' "$LOG_FILE"

"${root_command[@]}" "$BINARY" "${arguments[@]}" 2>&1 | tee "$LOG_FILE"

printf 'LibDB run completed.\n'
