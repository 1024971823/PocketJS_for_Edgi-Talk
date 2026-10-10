#!/usr/bin/env bash
# Run the checks that do not require a board. Full BSP checks are enabled when available.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/.." && pwd)"
cd "$PROJECT_ROOT"

MODE=auto
case "${1:-}" in
    "")
        ;;
    --source-only|--test-only)
        MODE="${1#--}"
        shift
        ;;
    *)
        echo "usage: $0 [--source-only|--test-only]" >&2
        exit 2
        ;;
esac

if [[ "$MODE" == "source-only" || "$MODE" == "test-only" ]]; then
    python3 "$HERE/check_dependencies.py" --source-only
    FULL_BUILD=0
elif [[ -d "$PROJECT_ROOT/rt-thread" || -d "$PROJECT_ROOT/../../rt-thread" ]]; then
    python3 "$HERE/check_dependencies.py"
    FULL_BUILD=1
else
    echo "full BSP is not present: validating source-only dependency metadata"
    python3 "$HERE/check_dependencies.py" --source-only
    FULL_BUILD=0
fi

python3 "$HERE/check_contracts.py"
python3 -m py_compile tools/edgitalk-companion/*.py tools/bench/*.py tools/*.py
python3 "$HERE/check_paths.py"
python3 -m unittest discover -s tools/edgitalk-companion -p 'test_*.py' -v

mkdir -p /tmp/edgitalk-ci
gcc -std=c11 -Wall -Wextra -Werror -pedantic -Iapplications/pocketjs/include \
    tools/preview/synth_test.c applications/pocketjs/pocketjs_synth.c \
    -lm -o /tmp/edgitalk-ci/synth_test
printf '0 0 60 2 100\n0 1 36 4 90\n0 2 0 1 100\n' > /tmp/edgitalk-ci/events.txt
/tmp/edgitalk-ci/synth_test 120 /tmp/edgitalk-ci/events.txt /tmp/edgitalk-ci/synth.wav 1

if [[ "$MODE" == "test-only" ]]; then
    echo "test checks passed"
    exit 0
fi

if (( FULL_BUILD )); then
    "$HERE/build.sh" --no-ui -n "$@"
else
    echo "scons dry-run skipped: full BSP is not present in this source-only checkout"
fi
echo "CI checks passed"
