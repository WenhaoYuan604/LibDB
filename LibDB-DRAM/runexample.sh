#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Run one LibDB-DRAM load/warmup/test sequence on a disposable SSD.
Usage: ./runexample.sh [--help]

Required:
  LIBDB_ALLOW_DESTRUCTIVE=1  Allow writes to the dedicated experiment SSD.
  PCI_ADDR=DDDD:BB:DD.F      NVMe controller PCI address.
  WORKLOAD_DIR=/path        Directory containing warm.txt and ycsbc.txt;
                           alternatively set WARM_FILE and TEST_FILE.

Configuration (defaults):
  THREADS=16                Server workers.
  LOAD_INJECTORS=8          Client injectors.
  TASKS_PER_THREAD=16       Outstanding tasks per worker.
  KV_SIZE=1024              Record size in bytes.
  KV_COUNT=100000000        Load count used for queue sizing.
  CACHE_SIZE_MB=16384       DRAM cache capacity in MiB.
  BOUNDARY_SIZE=1000        Initial boundary sample size.
  WAL=1                    Enable the per-worker PUT WAL.
  WAL_SIZE_MB_PER_WORKER=8192  WAL capacity in MiB (multiple of 4, at least 8).
  WAL_MAX_INFLIGHT=1        Inflight WAL batches (1..16).
  WAL_WAIT_US=10            Positive WAL batch wait, in microseconds.
  PHASE_IO_STATS=1          Emit phase I/O and WA statistics.
  PRECONDITION_A_FILE=      Optional A trace before the measured workload.
  RECOVERY=0               Rebuild from existing compatible SSD/WAL data.
  RECOVERY_SCAN=first-empty Recovery scan mode: first-empty or full.
  BUILD=1                  Build the libdb target before running.
  BUILD_JOBS=<nproc>        Build parallelism.
  SPDK_ROOT=../../spdk      Built SPDK source tree, relative to this script.
  BUILD_DIR=./build         Build directory, relative to this script.
  RESULT_DIR=./results      Log/result directory, relative to this script.
  USE_SUDO=1               Use sudo for LibDB and optional SPDK setup.
  PREPARE_SPDK=0            Explicitly enable SPDK scripts/setup.sh.

The default test is YCSB C. Set TEST_FILE to select another trace.
Recovery requires WAL=1 and matching layout/WAL settings. first-empty assumes
contiguous occupied physical segments; use full when this assumption is invalid.
The script never generates traces, scans their contents, clears the SSD, accesses
PMEM, tunes CPU/NUMA, or runs an experiment matrix. Device writes destroy data.
EOF
}
die() { printf 'error: %s\n' "$*" >&2; exit 2; }
flag() { [[ "${!1}" == 0 || "${!1}" == 1 ]] || die "$1 must be 0 or 1"; }
positive() { [[ "${!1}" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"; }
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then usage; exit 0; fi
[[ $# == 0 ]] || die 'unknown argument; use --help'
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SPDK_ROOT=${SPDK_ROOT:-"$SCRIPT_DIR/../../spdk"}
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/build"}
RESULT_DIR=${RESULT_DIR:-"$SCRIPT_DIR/results"}
WORKLOAD_DIR=${WORKLOAD_DIR:-}
WARM_FILE=${WARM_FILE:-${WORKLOAD_DIR:+$WORKLOAD_DIR/warm.txt}}
TEST_FILE=${TEST_FILE:-${WORKLOAD_DIR:+$WORKLOAD_DIR/ycsbc.txt}}
PRECONDITION_A_FILE=${PRECONDITION_A_FILE:-}
PCI_ADDR=${PCI_ADDR:-}
THREADS=${THREADS:-16}
LOAD_INJECTORS=${LOAD_INJECTORS:-8}
TASKS_PER_THREAD=${TASKS_PER_THREAD:-16}
KV_SIZE=${KV_SIZE:-1024}
KV_COUNT=${KV_COUNT:-100000000}
CACHE_SIZE_MB=${CACHE_SIZE_MB:-16384}
BOUNDARY_SIZE=${BOUNDARY_SIZE:-1000}
WAL=${WAL:-1}
WAL_SIZE_MB_PER_WORKER=${WAL_SIZE_MB_PER_WORKER:-8192}
WAL_MAX_INFLIGHT=${WAL_MAX_INFLIGHT:-1}
WAL_WAIT_US=${WAL_WAIT_US:-10}
PHASE_IO_STATS=${PHASE_IO_STATS:-1}
RECOVERY=${RECOVERY:-0}
RECOVERY_SCAN=${RECOVERY_SCAN:-first-empty}
BUILD=${BUILD:-1}
BUILD_JOBS=${BUILD_JOBS:-$(nproc)}
USE_SUDO=${USE_SUDO:-1}
PREPARE_SPDK=${PREPARE_SPDK:-0}
for name in WAL PHASE_IO_STATS RECOVERY BUILD USE_SUDO PREPARE_SPDK; do flag "$name"; done
for name in THREADS LOAD_INJECTORS TASKS_PER_THREAD KV_SIZE KV_COUNT BOUNDARY_SIZE WAL_SIZE_MB_PER_WORKER WAL_MAX_INFLIGHT WAL_WAIT_US BUILD_JOBS; do
    positive "$name"
    value=${!name}
    [[ ${#value} -le 9 ]] || die "$name is too large"
done
[[ "$CACHE_SIZE_MB" =~ ^(0|[1-9][0-9]*)$ && ${#CACHE_SIZE_MB} -le 9 ]] || die 'invalid CACHE_SIZE_MB'
((THREADS + LOAD_INJECTORS + 1 <= 128)) || die 'too many workers/injectors'
((KV_SIZE >= 52 && KV_SIZE <= 32768)) || die 'KV_SIZE must be between 52 and 32768'
((WAL == 0 || (THREADS <= 31 && KV_SIZE <= 4096))) || die 'WAL requires at most 31 workers and KV_SIZE <= 4096'
((WAL_SIZE_MB_PER_WORKER >= 8 && WAL_SIZE_MB_PER_WORKER % 4 == 0)) || die 'invalid WAL_SIZE_MB_PER_WORKER'
((WAL_MAX_INFLIGHT >= 1 && WAL_MAX_INFLIGHT <= 16)) || die 'WAL_MAX_INFLIGHT must be 1..16'
[[ "$RECOVERY_SCAN" == first-empty || "$RECOVERY_SCAN" == full ]] || die 'invalid RECOVERY_SCAN'
[[ "$RECOVERY" == 0 || "$WAL" == 1 ]] || die 'recovery requires WAL=1'
[[ "$PCI_ADDR" =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[01][[:xdigit:]]\.[0-7]$ ]] || die 'invalid PCI_ADDR'
PCI_ADDR=${PCI_ADDR,,}
[[ "${LIBDB_ALLOW_DESTRUCTIVE:-0}" == 1 ]] || die 'set LIBDB_ALLOW_DESTRUCTIVE=1 for the disposable SSD'
for command in realpath readlink findmnt tee mktemp; do command -v "$command" >/dev/null || die "missing $command"; done
for name in BUILD_DIR RESULT_DIR; do
    value=${!name}; [[ "$value" == /* ]] || printf -v "$name" '%s/%s' "$SCRIPT_DIR" "$value"
done
[[ "$SPDK_ROOT" == /* ]] || SPDK_ROOT="$SCRIPT_DIR/$SPDK_ROOT"
for name in WARM_FILE TEST_FILE PRECONDITION_A_FILE; do
    value=${!name}
    [[ "$name" != PRECONDITION_A_FILE || -n "$value" ]] || continue
    [[ -n "$value" && -f "$value" && -r "$value" ]] || die "$name must name a readable regular file"
    printf -v "$name" '%s' "$(realpath -- "$value")"
done
[[ -z "$PRECONDITION_A_FILE" || "$TEST_FILE" != *Production* ]] || die 'A preconditioning requires a YCSB test trace'
[[ -d "/sys/bus/pci/devices/$PCI_ADDR" ]] || die 'PCI controller does not exist'


declare -A seen_devices=()
check_block_device() {
    local path child
    path=$(readlink -f -- "$1") || die 'cannot resolve block device'
    [[ "$path" != *"/$PCI_ADDR/"* ]] || die 'workload/output filesystem or mounted storage uses the experiment SSD'
    [[ -z "${seen_devices[$path]:-}" ]] || return 0
    seen_devices[$path]=1
    for child in "$path"/slaves/*; do [[ ! -e "$child" ]] || check_block_device "$child"; done
}
check_file_device() {
    local device
    device=$(findmnt -rn -o MAJ:MIN --target "$1") || die "cannot identify filesystem for $1"
    [[ "$device" =~ ^[0-9]+:[0-9]+$ ]] || die 'ambiguous filesystem device'
    if [[ -e "/sys/dev/block/$device" ]]; then check_block_device "/sys/dev/block/$device"; fi
}
for file in "$WARM_FILE" "$TEST_FILE" "$PRECONDITION_A_FILE"; do [[ -z "$file" ]] || check_file_device "$file"; done
for directory in "$RESULT_DIR" "$BUILD_DIR"; do
    ancestor=$(realpath -m -- "$directory")
    while [[ ! -e "$ancestor" ]]; do ancestor=$(dirname -- "$ancestor"); done
    check_file_device "$ancestor"
done
mounted_devices=$(findmnt -rn -o MAJ:MIN) || die 'cannot inspect mounted filesystems'
while read -r device; do
    [[ ! -e "/sys/dev/block/$device" ]] || check_block_device "/sys/dev/block/$device"
done <<< "$mounted_devices"

while read -r swap rest; do
    [[ "$swap" != Filename && -e "$swap" ]] || continue
    if [[ -b "$swap" ]]; then
        swap_device=$(realpath -- "$swap")
        check_block_device "/sys/class/block/${swap_device##*/}"
    else
        check_file_device "$swap"
    fi
done < /proc/swaps

root_command=()
if [[ "$USE_SUDO" == 1 && "$EUID" != 0 ]]; then
    command -v sudo >/dev/null || die 'sudo is required'; root_command=(sudo)
fi
if [[ "$BUILD" == 1 ]]; then
    command -v cmake >/dev/null || die 'cmake is required'
    [[ -d "$SPDK_ROOT" ]] || die 'SPDK_ROOT does not exist'
    export PKG_CONFIG_PATH="$SPDK_ROOT/build/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DSPDK_ROOT="$SPDK_ROOT"
    cmake --build "$BUILD_DIR" --target libdb --parallel "$BUILD_JOBS"
fi
BINARY="$BUILD_DIR/test"
[[ -x "$BINARY" ]] || die "executable not found: $BINARY"
if [[ "$PREPARE_SPDK" == 1 ]]; then
    [[ -x "$SPDK_ROOT/scripts/setup.sh" ]] || die 'SPDK setup script not found'
    "${root_command[@]}" env HUGE_EVEN_ALLOC=yes PCI_ALLOWED="$PCI_ADDR" "$SPDK_ROOT/scripts/setup.sh"
fi
driver=$(readlink -f -- "/sys/bus/pci/devices/$PCI_ADDR/driver") || die 'controller has no driver'
case "${driver##*/}" in vfio-pci|uio_pci_generic|igb_uio) ;; *) die 'bind the SSD to an SPDK-compatible driver first' ;; esac
mkdir -p -- "$RESULT_DIR"
OUTPUT_FILE=$(mktemp "$RESULT_DIR/result_$(date +%Y%m%d_%H%M%S)_XXXXXX.txt")
LOG_FILE="${OUTPUT_FILE%.txt}.log"
arguments=("--thread=$THREADS" "--kvsize=$KV_SIZE" "--kvs=$KV_COUNT" "--cachesize=$CACHE_SIZE_MB"
    "--load-injectors=$LOAD_INJECTORS" "--tasks-per-thread=$TASKS_PER_THREAD" "--boundary-size=$BOUNDARY_SIZE"
    "--pci-addr=$PCI_ADDR" "--warm=$WARM_FILE" "--test=$TEST_FILE" "--output=$OUTPUT_FILE"
    "--wal=$WAL" "--wal-size-mb-per-worker=$WAL_SIZE_MB_PER_WORKER" "--wal-max-inflight=$WAL_MAX_INFLIGHT"
    "--wal-wait-us=$WAL_WAIT_US" "--phase-io-stats=$PHASE_IO_STATS" "--recovery=$RECOVERY" "--recovery-scan=$RECOVERY_SCAN")
[[ -z "$PRECONDITION_A_FILE" ]] || arguments+=("--precondition-a=$PRECONDITION_A_FILE")
printf 'Starting LibDB-DRAM. Results: %s\nLog: %s\n' "$OUTPUT_FILE" "$LOG_FILE"
"${root_command[@]}" "$BINARY" "${arguments[@]}" 2>&1 | tee "$LOG_FILE"
printf 'LibDB-DRAM run completed.\n'
