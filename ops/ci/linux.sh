#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
image=llmgate-build
docker build -q -t "$image" "$root/ops/ci" >/dev/null
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/src" -w /src "$image" \
  sh -c 'git config --global --add safe.directory "*" && cmake --preset linux && cmake --build --preset linux && ctest --preset linux "$@"' sh "$@"
