#!/usr/bin/env bash
# Build and run the desktop preview harness against the embedded PocketJS package.
#   tools/preview/build.sh [perfect|idle] [songIndex] [frames]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$HERE/../.." && pwd)"
WORK="$(cd "$PROJECT/../../.." && pwd)"      # .../b/work
POCKET="${POCKETJS_ROOT:-$WORK/pocketjs}"
QJS="${QUICKJS_ROOT:-$WORK/quickjs-ng}"
COMP="$POCKET/hosts/esp-idf/components"
OUT="${PREVIEW_OUT:-/tmp/pjs-preview}"
APP="$PROJECT/applications/pocketjs"
MODE="${1:-perfect}"
SONG="${2:-2}"
FRAMES="${3:-2000}"
mkdir -p "$OUT/shots"

if [ ! -f "$OUT/cargo/ui-core/release/libpocketjs_idf_ui_core.a" ]; then
  echo "build the host Rust archives first (see tools/preview/README.md)" >&2
  exit 1
fi

INCLUDES=(-I"$POCKET/hosts/esp-idf/tests/host/include" -I"$QJS" -I"$APP/generated")
for c in pocketjs_package pocketjs_guest pocketjs_ui_core pocketjs_ui_qjs pocketjs_render_rgb565; do
  INCLUDES+=(-I"$COMP/$c/include")
done

if [ ! -f "$OUT/quickjs.o" ] || [ "$QJS/quickjs.c" -nt "$OUT/quickjs.o" ]; then
  gcc -O2 -std=gnu11 -D_GNU_SOURCE -w -c "$QJS/quickjs.c" -I"$QJS" -o "$OUT/quickjs.o"
  for f in libregexp libunicode dtoa; do
    gcc -O2 -std=gnu11 -D_GNU_SOURCE -w -c "$QJS/$f.c" -I"$QJS" -o "$OUT/$f.o"
  done
fi

gcc -c -w "${INCLUDES[@]}" "$APP/generated/pocketjs_package_edgitalk_smoke.S" -o "$OUT/pkg_asm.o"

gcc -O1 -g -std=gnu17 -D_GNU_SOURCE -w -include "$APP/include/pocketjs_qjs_ng_compat.h" "${INCLUDES[@]}" \
  "$HERE/harness.c" "$OUT/pkg_asm.o" \
  "$APP/generated/pocketjs_package_edgitalk_smoke.c" \
  "$COMP/pocketjs_package/src/package.c" "$COMP/pocketjs_guest/src/guest.c" \
  "$COMP/pocketjs_ui_core/src/ui_core.c" "$COMP/pocketjs_ui_qjs/src/ui_qjs.c" \
  "$COMP/pocketjs_render_rgb565/src/render_rgb565.c" \
  "$OUT/quickjs.o" "$OUT/libregexp.o" "$OUT/libunicode.o" "$OUT/dtoa.o" \
  "$OUT/cargo/render-rgb565/release/libpocketjs_idf_render_rgb565.a" \
  "$OUT/cargo/ui-core/release/libpocketjs_idf_ui_core.a" \
  -lm -lpthread -ldl -o "$OUT/harness"

bun_bin="${BUN_BIN:-$HOME/.bun/bin/bun}"
if [[ ! -x "$bun_bin" ]]; then bun_bin="$(command -v bun || true)"; fi
[[ -n "$bun_bin" ]] || { echo "bun not found; set BUN_BIN" >&2; exit 1; }
( cd "$HERE" && "$bun_bin" build scenario.ts --outfile "$OUT/scenario.js" --target browser --format iife >/dev/null )
{ echo "globalThis.__mode=\"$MODE\"; globalThis.__song=$SONG;"; cat "$HERE/prelude.js"; } > "$OUT/prelude.js"
rm -f "$OUT/shots/"*.ppm "$OUT/shots/"*.png
"$OUT/harness" "$OUT/prelude.js" "$OUT/scenario.js" "$OUT/shots" "$FRAMES"
python3 "$HERE/topng.py" "$OUT/shots/"*.ppm
