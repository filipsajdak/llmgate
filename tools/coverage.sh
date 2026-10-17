#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
cmake --preset coverage >/dev/null
cmake --build --preset coverage
find build/coverage -name '*.gcda' -delete
ctest --preset coverage
gcovr --gcov-executable gcov-16 -r . --filter 'src/' --json build/coverage/cov.json build/coverage
python3 tools/crap.py build/coverage/cov.json src
