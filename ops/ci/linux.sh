#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
image=llmgate-build
preset=${PRESET:-linux}
case "$preset" in
  *tsan*) relax="--security-opt seccomp=unconfined" ;;
  *) relax="" ;;
esac
case "$preset" in
  *asan*) default_jobs=2 ;;
  *) default_jobs=4 ;;
esac
docker build -q -t "$image" "$root/ops/ci" >/dev/null
docker run --rm $relax --cap-add SYS_PTRACE --user "$(id -u):$(id -g)" -e HOME=/tmp -e PRESET="$preset" -e CMAKE_ARGS="${CMAKE_ARGS:-}" -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$default_jobs}" -v "$root:/src" -w /src "$image" \
  sh -c 'git config --global --add safe.directory "*" && cmake --preset "$PRESET" $CMAKE_ARGS && cmake --build --preset "$PRESET" && ctest --preset "$PRESET" "$@"' sh "$@"
