#!/usr/bin/env bash
# Flash this project explicitly; never inherit a different project's default.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/.." && pwd)"
WORK_ROOT="$(cd "$PROJECT_ROOT/../../.." && pwd)"
FLASH_TOOL="${EDGI_TALK_FLASH_TOOL:-$WORK_ROOT/flash-xiaozhi-daplink.sh}"
[[ -f "$FLASH_TOOL" ]] || { echo "flash tool not found: $FLASH_TOOL" >&2; exit 1; }
M55_PROJECT="Edgi_Talk_M55_PocketJS" exec "$FLASH_TOOL" "$@"
