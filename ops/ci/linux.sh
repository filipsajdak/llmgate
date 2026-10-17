#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
image=llmgate-build
preset=${PRESET:-linux}
case "$preset" in
  *tsan*) relax="--security-opt seccomp=unconfined" ;;
  *) relax="" ;;
esac
docker build -q -t "$image" "$root/ops/ci" >/dev/null
docker run --rm $relax --cap-add SYS_PTRACE --user "$(id -u):$(id -g)" -e HOME=/tmp -e PRESET="$preset" -v "$root:/src" -w /src "$image" \
  sh -c 'git config --global --add safe.directory "*" && cmake --preset "$PRESET" && cmake --build --preset "$PRESET" && ctest --preset "$PRESET" "$@"' sh "$@"
