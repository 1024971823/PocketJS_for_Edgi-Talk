#!/usr/bin/env bash
# Reproducible firmware build. Override POCKETJS_ROOT/QUICKJS_ROOT/RTT_EXEC_PATH as needed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/.." && pwd)"
WORK_ROOT="$(cd "$PROJECT_ROOT/../../.." && pwd)"
SDK_ROOT="$(cd "$PROJECT_ROOT/../.." && pwd)"
POCKETJS_ROOT="${POCKETJS_ROOT:-$WORK_ROOT/pocketjs}"
QUICKJS_ROOT="${QUICKJS_ROOT:-$WORK_ROOT/quickjs-ng}"
APP_NAME="edgitalk-m55-smoke"
APP_DIR="$POCKETJS_ROOT/apps/$APP_NAME"
GENERATOR="$POCKETJS_ROOT/hosts/esp-idf/components/pocketjs_package/tools/embed_package.py"
PROFILE="$APP_DIR/pocket.host.json"
PACKAGE="$APP_DIR/dist/$APP_NAME.pocket"

if [[ "${1:-}" == "--no-ui" ]]; then
    shift
    BUILD_UI=0
else
    BUILD_UI=1
fi

python_bin="${EDGI_TALK_PYTHON:-}"
if [[ -z "$python_bin" ]]; then
    if [[ -x "$WORK_ROOT/edgi-talk-py312/bin/python" ]]; then
        python_bin="$WORK_ROOT/edgi-talk-py312/bin/python"
    else
        python_bin="$(command -v python3)"
    fi
fi
if [[ -z "${RTT_EXEC_PATH:-}" ]]; then
    if [[ -x "$WORK_ROOT/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gcc" ]]; then
        RTT_EXEC_PATH="$WORK_ROOT/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin"
    else
        toolchain_gcc="$(command -v arm-none-eabi-gcc || true)"
        [[ -n "$toolchain_gcc" ]] || { echo "arm-none-eabi-gcc not found; set RTT_EXEC_PATH" >&2; exit 1; }
        RTT_EXEC_PATH="$(dirname "$toolchain_gcc")"
    fi
fi
export EDGI_TALK_HOME="$SDK_ROOT" EDGI_TALK_PYTHON="$python_bin" RTT_EXEC_PATH
export POCKETJS_ROOT QUICKJS_ROOT
export PATH="$(dirname "$python_bin"):$RTT_EXEC_PATH:$PATH"

for path in "$POCKETJS_ROOT" "$QUICKJS_ROOT" "$GENERATOR"; do
    [[ -e "$path" ]] || { echo "missing dependency: $path" >&2; exit 1; }
done

if (( BUILD_UI )); then
    bun_bin="${BUN_BIN:-$HOME/.bun/bin/bun}"
    [[ -x "$bun_bin" ]] || bun_bin="$(command -v bun || true)"
    [[ -n "$bun_bin" ]] || { echo "bun not found; set BUN_BIN or use --no-ui" >&2; exit 1; }
    (cd "$POCKETJS_ROOT" && "$bun_bin" tools/pocket.ts build \
        --host-profile "apps/$APP_NAME/pocket.host.json" \
        --manifest "apps/$APP_NAME/pocket.json")
fi

"$python_bin" "$HERE/embed_package.py" \
    --package "$PACKAGE" --host-profile "$PROFILE" --name edgitalk_smoke \
    --output-dir "$PROJECT_ROOT/applications/pocketjs/generated" \
    --project-root "$PROJECT_ROOT" --generator "$GENERATOR"
"$python_bin" "$HERE/check_dependencies.py"

cd "$PROJECT_ROOT"
exec "$python_bin" -m SCons "$@"
