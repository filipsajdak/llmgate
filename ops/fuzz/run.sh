#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
target=${1:?usage: run.sh <http_parser|sse_parser|jsonscan|guard|json|routing|settings|launcher|usage|schedule|proxy|eventstream> [seconds]}
seconds=${2:-60}
docker build -q -t llmgate-build "$root/ops/ci" >/dev/null
docker build -q -t llmgate-fuzz -f "$root/ops/ci/Dockerfile.fuzz" "$root/ops/ci" >/dev/null
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e AFL_SKIP_CPUFREQ=1 -e AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
  -e AFL_NO_UI=1 -e AFL_USE_ASAN=1 -e AFL_USE_UBSAN=1 -e ASAN_OPTIONS=abort_on_error=1:symbolize=0:detect_leaks=0 -e UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1 -v "$root:/src" -w /src llmgate-fuzz sh -c "
    git config --global --add safe.directory '*' &&
    rm -rf build/fuzz && cmake --preset fuzz >/dev/null && cmake --build --preset fuzz &&
    mkdir -p build/fuzz/findings/$target &&
    afl-fuzz -i tests/fuzz/$target/corpus -o build/fuzz/findings/$target -m none -V $seconds -- build/fuzz/fuzz_$target @@ &&
    ls build/fuzz/findings/$target/default/crashes build/fuzz/findings/$target/default/hangs"
