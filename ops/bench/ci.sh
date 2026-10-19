#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:-bench}
case "$mode" in
  bench)
    preset=linux
    options=""
    command='python3 -B ops/bench/bench.py --binary /src/build/linux/llmgate --seconds 3 --concurrency 1 8 --body-kb 2 100 --out /src/build/bench-ci >/dev/null && python3 -B ops/bench/compare.py "$(ls -t /src/build/bench-ci/*.json | head -1)"'
    ;;
  soak)
    preset=linux-asan
    options="-e ASAN_OPTIONS=quarantine_size_mb=1:detect_leaks=1"
    command='python3 -B ops/bench/soak.py --binary /src/build/linux-asan/llmgate --minutes "${SOAK_MINUTES:-3}"'
    ;;
  *) echo "usage: ops/bench/ci.sh [bench|soak]" >&2; exit 2 ;;
esac
PRESET=$preset "$root/ops/ci/linux.sh" -N >/dev/null
rm -rf "$root/build/bench-ci"
docker run --rm --security-opt seccomp=unconfined --cap-add SYS_PTRACE --user "$(id -u):$(id -g)" -e HOME=/tmp -e SOAK_MINUTES="${SOAK_MINUTES:-}" $options \
  -v "$root:/src" -w /src llmgate-build sh -c "$command"
