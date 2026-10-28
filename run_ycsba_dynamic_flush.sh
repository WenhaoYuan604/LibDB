#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 -u "$SCRIPT_DIR/tools/run_ycsba_dynamic_flush.py" "$@"
