#!/usr/bin/env bash
set -Eeuo pipefail
# Number of complete 8-case rounds; override with REPEATS=N.
REPEATS=${REPEATS:-1}
export REPEATS
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 -u "$SCRIPT_DIR/tools/run_ycsb_ae_kvsize.py" "$@"
