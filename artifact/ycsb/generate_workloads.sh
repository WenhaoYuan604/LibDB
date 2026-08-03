#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
Generate the LibDB warm, YCSB A, YCSB C, and YCSB E text traces.

Usage:
  YCSB_ALLOW_DESTRUCTIVE=1 \
  YCSB_DB_DIR=/path/to/new/disposable/rocksdb-directory \
  ./artifact/ycsb/generate_workloads.sh

The RocksDB directory must not already exist. The script creates it and, unless
KEEP_DB=1 is set, removes only that newly created directory on exit.

Common overrides:
  OUTPUT_DIR=<this-directory>/generated
  YCSB_REPO_URL=https://github.com/haslaboratory/YCSB-tracegen.git
  YCSB_COMMIT=314c6490956e8deb9f9ee71ee2bd48d1bfd2e510
  YCSB_THREADS=32
  RECORD_COUNT=100000000
  YCSBA_OPERATION_COUNT=100000000
  YCSBC_OPERATION_COUNT=100000000
  YCSBE_OPERATION_COUNT=20000000
  OVERWRITE_OUTPUT=0
  KEEP_DB=0

For a quick generator check, set all four count variables to 1000.
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
YCSB_REPO_URL=${YCSB_REPO_URL:-https://github.com/haslaboratory/YCSB-tracegen.git}
YCSB_COMMIT=${YCSB_COMMIT:-314c6490956e8deb9f9ee71ee2bd48d1bfd2e510}
YCSB_DB_DIR=${YCSB_DB_DIR:-}
OUTPUT_DIR=${OUTPUT_DIR:-"${SCRIPT_DIR}/generated"}
YCSB_THREADS=${YCSB_THREADS:-32}
RECORD_COUNT=${RECORD_COUNT:-100000000}
YCSBA_OPERATION_COUNT=${YCSBA_OPERATION_COUNT:-100000000}
YCSBC_OPERATION_COUNT=${YCSBC_OPERATION_COUNT:-100000000}
YCSBE_OPERATION_COUNT=${YCSBE_OPERATION_COUNT:-20000000}
OVERWRITE_OUTPUT=${OVERWRITE_OUTPUT:-0}
KEEP_DB=${KEEP_DB:-0}
YCSB_ALLOW_DESTRUCTIVE=${YCSB_ALLOW_DESTRUCTIVE:-0}

require_positive_integer YCSB_THREADS "$YCSB_THREADS"
require_positive_integer RECORD_COUNT "$RECORD_COUNT"
require_positive_integer YCSBA_OPERATION_COUNT "$YCSBA_OPERATION_COUNT"
require_positive_integer YCSBC_OPERATION_COUNT "$YCSBC_OPERATION_COUNT"
require_positive_integer YCSBE_OPERATION_COUNT "$YCSBE_OPERATION_COUNT"
require_flag OVERWRITE_OUTPUT "$OVERWRITE_OUTPUT"
require_flag KEEP_DB "$KEEP_DB"
[[ "$YCSB_ALLOW_DESTRUCTIVE" == "1" ]] || die \
    "set YCSB_ALLOW_DESTRUCTIVE=1 after choosing a disposable YCSB_DB_DIR"
[[ -n "$YCSB_DB_DIR" ]] || die "set YCSB_DB_DIR to a new disposable RocksDB directory"

for command_name in git mvn java realpath sha256sum wc mktemp; do
    command -v "$command_name" >/dev/null 2>&1 || die "$command_name is required"
done

YCSB_DB_DIR=$(realpath -m -- "$YCSB_DB_DIR")
OUTPUT_DIR=$(realpath -m -- "$OUTPUT_DIR")
[[ "$YCSB_DB_DIR" != "/" ]] || die "refusing to use / as YCSB_DB_DIR"
[[ ! -e "$YCSB_DB_DIR" ]] || die "YCSB_DB_DIR must not already exist: $YCSB_DB_DIR"

case "${OUTPUT_DIR}/" in
    "${YCSB_DB_DIR}/"*) die "OUTPUT_DIR must not be inside YCSB_DB_DIR" ;;
esac
case "${YCSB_DB_DIR}/" in
    "${OUTPUT_DIR}/"*) die "YCSB_DB_DIR must not be inside OUTPUT_DIR" ;;
esac

mkdir -p -- "$OUTPUT_DIR"
outputs=(warm.txt ycsba.txt ycsbc.txt ycsbe.txt MANIFEST.txt SHA256SUMS)
for output_name in "${outputs[@]}"; do
    if [[ -e "${OUTPUT_DIR}/${output_name}" && "$OVERWRITE_OUTPUT" != "1" ]]; then
        die "output already exists: ${OUTPUT_DIR}/${output_name}; set OVERWRITE_OUTPUT=1 to replace it"
    fi
done
if [[ "$OVERWRITE_OUTPUT" == "1" ]]; then
    for output_name in "${outputs[@]}"; do
        rm -f -- "${OUTPUT_DIR}/${output_name}"
    done
fi

BUILD_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/libdb-ycsb-build.XXXXXX")
SOURCE_DIR="${BUILD_ROOT}/YCSB-tracegen"
db_created=0

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ $db_created -eq 1 && "$KEEP_DB" != "1" ]]; then
        printf 'Removing generator RocksDB directory: %s\n' "$YCSB_DB_DIR"
        rm -rf -- "$YCSB_DB_DIR"
    fi
    if [[ -n "${BUILD_ROOT:-}" && -d "$BUILD_ROOT" ]]; then
        rm -rf -- "$BUILD_ROOT"
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'Cloning YCSB-tracegen at %s\n' "$YCSB_COMMIT"
git clone --quiet "$YCSB_REPO_URL" "$SOURCE_DIR"
git -C "$SOURCE_DIR" checkout --quiet --detach "$YCSB_COMMIT"
actual_commit=$(git -C "$SOURCE_DIR" rev-parse HEAD)
[[ "$actual_commit" == "$YCSB_COMMIT" ]] || die \
    "unexpected YCSB commit: expected $YCSB_COMMIT, got $actual_commit"

git -C "$SOURCE_DIR" apply --check "${SCRIPT_DIR}/patches/0001-log-scan-operations.patch"
git -C "$SOURCE_DIR" apply "${SCRIPT_DIR}/patches/0001-log-scan-operations.patch"

printf 'Building the YCSB RocksDB binding.\n'
mvn -q -f "${SOURCE_DIR}/pom.xml" -pl site.ycsb:rocksdb-binding -am \
    clean package -DskipTests

mkdir -p -- "$YCSB_DB_DIR"
db_created=1

run_trace() {
    local mode=$1
    local workload_file=$2
    local operation_count=$3
    local output_file=$4
    shift 4
    local -a extra_options=("$@")

    rm -f -- "${SOURCE_DIR}/test.txt"
    printf 'Generating %s\n' "$output_file"
    (
        cd "$SOURCE_DIR"
        bin/ycsb "$mode" rocksdb -s -threads "$YCSB_THREADS" \
            -P "$workload_file" \
            -p "recordcount=${RECORD_COUNT}" \
            -p "operationcount=${operation_count}" \
            -p "rocksdb.dir=${YCSB_DB_DIR}" \
            "${extra_options[@]}"
    )
    [[ -s "${SOURCE_DIR}/test.txt" ]] || die "YCSB did not create a non-empty test.txt for $output_file"
    mv -- "${SOURCE_DIR}/test.txt" "${OUTPUT_DIR}/${output_file}"
}

run_trace load "${SCRIPT_DIR}/workloads/workloadc.properties" "$RECORD_COUNT" warm.txt \
    -p rocksdb.disable_wal=true \
    -p rocksdb.target_file_size_base=512m \
    -p rocksdb.write_buffer_size=512m \
    -p rocksdb.max_write_buffer_number=8 \
    -p rocksdb.max_background_compactions=8 \
    -p rocksdb.max_background_flushes=4

run_trace run "${SCRIPT_DIR}/workloads/workloada.properties" \
    "$YCSBA_OPERATION_COUNT" ycsba.txt
run_trace run "${SCRIPT_DIR}/workloads/workloadc.properties" \
    "$YCSBC_OPERATION_COUNT" ycsbc.txt
run_trace run "${SCRIPT_DIR}/workloads/workloade.properties" \
    "$YCSBE_OPERATION_COUNT" ycsbe.txt

(
    cd "$OUTPUT_DIR"
    sha256sum warm.txt ycsba.txt ycsbc.txt ycsbe.txt > SHA256SUMS
    {
        printf 'ycsb_repository=%s\n' "$YCSB_REPO_URL"
        printf 'ycsb_commit=%s\n' "$YCSB_COMMIT"
        printf 'threads=%s\n' "$YCSB_THREADS"
        printf 'record_count=%s\n' "$RECORD_COUNT"
        printf 'ycsba_operation_count=%s\n' "$YCSBA_OPERATION_COUNT"
        printf 'ycsbc_operation_count=%s\n' "$YCSBC_OPERATION_COUNT"
        printf 'ycsbe_operation_count=%s\n' "$YCSBE_OPERATION_COUNT"
        wc -l warm.txt ycsba.txt ycsbc.txt ycsbe.txt
    } > MANIFEST.txt
)

printf 'Generated workloads and checksums in %s\n' "$OUTPUT_DIR"
