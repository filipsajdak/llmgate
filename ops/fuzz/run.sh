#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
target=${1:?usage: run.sh <http_parser|sse_parser> [seconds]}
seconds=${2:-60}
docker build -q -t llmgate-build "$root/ops/ci" >/dev/null
docker build -q -t llmgate-fuzz -f "$root/ops/ci/Dockerfile.fuzz" "$root/ops/ci" >/dev/null
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e AFL_SKIP_CPUFREQ=1 -e AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
  -e AFL_NO_UI=1 -v "$root:/src" -w /src llmgate-fuzz sh -c "
    git config --global --add safe.directory '*' &&
    cmake --preset fuzz >/dev/null && cmake --build --preset fuzz &&
    mkdir -p build/fuzz/findings/$target &&
    afl-fuzz -i tests/fuzz/$target/corpus -o build/fuzz/findings/$target -V $seconds -- build/fuzz/fuzz_$target @@ &&
    ls build/fuzz/findings/$target/default/crashes build/fuzz/findings/$target/default/hangs"
