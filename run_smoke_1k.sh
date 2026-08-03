#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
Run the 1 KiB LibDB smoke test and remove its PMEM files before and after it.

Usage:
  ./run_smoke_1k.sh

Required environment:
  LIBDB_ALLOW_DESTRUCTIVE=1  Acknowledge that the experiment SSD is disposable.
  PCI_ADDR=DDDD:BB:DD.F      Dedicated experiment NVMe controller PCI BDF.
  PMEM_BASE=/mnt/pmem0/libdb Parent directory for smoke-test PMEM files.

Optional environment overrides:
  PMEM_DIR=$PMEM_BASE/libdb_clean_smoke_1k
  TIMEOUT_SECONDS=180
  KILL_AFTER_SECONDS=10
  USE_SUDO=1
  BINARY=<repo>/build/test
  WORKLOAD_DIR=/tmp/libdb-clean-smoke-workload
  OUTPUT_FILE=/tmp/libdb-clean-smoke-1k-result.txt
  LOG_FILE=/tmp/libdb-clean-smoke-1k.log

Only PMEM_DIR is removed. Other data below /mnt/whypmem is not touched.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
[[ $# -eq 0 ]] || die "unknown argument '$1'; use --help"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BINARY=${BINARY:-"${SCRIPT_DIR}/build/test"}
PCI_ADDR=${PCI_ADDR:-}
PMEM_BASE=${PMEM_BASE:-}
PMEM_DIR=${PMEM_DIR:-${PMEM_BASE:+${PMEM_BASE}/libdb_clean_smoke_1k}}
WORKLOAD_DIR=${WORKLOAD_DIR:-/tmp/libdb-clean-smoke-workload}
WARM_FILE=${WARM_FILE:-"${WORKLOAD_DIR}/warm.txt"}
TEST_FILE=${TEST_FILE:-"${WORKLOAD_DIR}/ycsbc.txt"}
OUTPUT_FILE=${OUTPUT_FILE:-/tmp/libdb-clean-smoke-1k-result.txt}
LOG_FILE=${LOG_FILE:-/tmp/libdb-clean-smoke-1k.log}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-180}
KILL_AFTER_SECONDS=${KILL_AFTER_SECONDS:-10}
USE_SUDO=${USE_SUDO:-1}
LIBDB_ALLOW_DESTRUCTIVE=${LIBDB_ALLOW_DESTRUCTIVE:-0}

THREADS=${THREADS:-1}
KV_SIZE=${KV_SIZE:-1024}
KV_COUNT=${KV_COUNT:-1000}
CACHE_SIZE_MB=${CACHE_SIZE_MB:-1}
LOAD_INJECTORS=${LOAD_INJECTORS:-1}
TASKS_PER_THREAD=${TASKS_PER_THREAD:-4}
BOUNDARY_SIZE=${BOUNDARY_SIZE:-1}

command -v realpath >/dev/null 2>&1 || die "realpath is required"
command -v timeout >/dev/null 2>&1 || die "timeout is required"
command -v tee >/dev/null 2>&1 || die "tee is required"
command -v awk >/dev/null 2>&1 || die "awk is required"
[[ -x "$BINARY" ]] || die "LibDB executable not found: $BINARY"
[[ "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "TIMEOUT_SECONDS must be positive"
[[ "$KILL_AFTER_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "KILL_AFTER_SECONDS must be positive"
[[ "$KV_COUNT" =~ ^[1-9][0-9]*$ ]] || die "KV_COUNT must be positive"
[[ "$USE_SUDO" == "0" || "$USE_SUDO" == "1" ]] || die "USE_SUDO must be 0 or 1"
[[ "$LIBDB_ALLOW_DESTRUCTIVE" == "1" ]] || die \
    "set LIBDB_ALLOW_DESTRUCTIVE=1 after verifying PCI_ADDR is a disposable experiment SSD"
[[ -n "$PCI_ADDR" ]] || die "set PCI_ADDR to the dedicated experiment NVMe controller"
[[ -n "$PMEM_BASE" ]] || die "set PMEM_BASE to the parent directory for smoke-test PMEM files"

PMEM_BASE=$(realpath -m -- "$PMEM_BASE")
PMEM_DIR=$(realpath -m -- "$PMEM_DIR")
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

cleanup_pmem() {
    printf 'Cleaning smoke-test PMEM directory: %s\n' "$PMEM_DIR"
    "${root_command[@]}" rm -rf -- "$PMEM_DIR"
}

finish() {
    local status=$?
    trap - EXIT INT TERM
    if ! cleanup_pmem; then
        printf 'warning: failed to clean PMEM directory: %s\n' "$PMEM_DIR" >&2
        [[ $status -ne 0 ]] || status=1
    fi
    exit "$status"
}

trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Remove leftovers from an interrupted run and recreate the exact test directory.
# The EXIT trap removes it again on success, failure, timeout, or interruption.
cleanup_pmem
"${root_command[@]}" mkdir -p -- "$PMEM_DIR"

mkdir -p -- "$WORKLOAD_DIR"
awk -v count="$KV_COUNT" 'BEGIN { for (i = 1; i <= count; ++i) print "I user" i " value" }' > "$WARM_FILE"
awk -v count="$KV_COUNT" 'BEGIN { for (i = 1; i <= count; ++i) print "R user" i }' > "$TEST_FILE"
"${root_command[@]}" rm -f -- "$OUTPUT_FILE"
rm -f -- "$LOG_FILE"

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

printf 'Starting LibDB 1 KiB smoke test.\n'
printf 'PCI device: %s\n' "$PCI_ADDR"
printf 'Log: %s\n' "$LOG_FILE"

set +e
"${root_command[@]}" timeout --signal=INT --kill-after="${KILL_AFTER_SECONDS}s" "${TIMEOUT_SECONDS}s" \
    "$BINARY" "${arguments[@]}" 2>&1 | tee "$LOG_FILE"
run_status=${PIPESTATUS[0]}
set -e

if [[ $run_status -eq 0 ]]; then
    printf 'LibDB smoke test completed successfully.\n'
elif [[ $run_status -eq 124 ]]; then
    printf 'LibDB smoke test reached the %ss timeout.\n' "$TIMEOUT_SECONDS" >&2
else
    printf 'LibDB smoke test failed with status %d.\n' "$run_status" >&2
fi

exit "$run_status"
