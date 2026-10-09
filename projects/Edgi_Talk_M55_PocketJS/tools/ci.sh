#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/.." && pwd)"
cd "$PROJECT_ROOT"
python3 "$HERE/check_dependencies.py"
python3 -m py_compile tools/edgitalk-companion/*.py tools/bench/*.py tools/*.py
python3 "$HERE/check_paths.py"
python3 -m unittest discover -s tools/edgitalk-companion -p 'test_*.py' -v
mkdir -p /tmp/edgitalk-ci
gcc -std=c11 -Wall -Wextra -Werror -pedantic -Iapplications/pocketjs/include \
    tools/preview/synth_test.c applications/pocketjs/pocketjs_synth.c \
    -lm -o /tmp/edgitalk-ci/synth_test
printf '0 0 60 2 100\n0 1 36 4 90\n0 2 0 1 100\n' > /tmp/edgitalk-ci/events.txt
/tmp/edgitalk-ci/synth_test 120 /tmp/edgitalk-ci/events.txt /tmp/edgitalk-ci/synth.wav 1
if [[ -d "$PROJECT_ROOT/rt-thread" || -d "$PROJECT_ROOT/../../rt-thread" ]]; then
    "$HERE/build.sh" --no-ui -n
else
    echo "scons dry-run skipped: full BSP is not present in this source-only checkout"
fi
python3 "$HERE/check_paths.py"
echo "CI checks passed"
