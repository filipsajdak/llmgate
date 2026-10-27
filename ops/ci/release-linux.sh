#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
version=${VERSION:?set VERSION to the release version, for example 1.2.3}
image=llmgate-build
docker build -q -t "$image" "$root/ops/ci" >/dev/null
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e VERSION="$version" -v "$root:/src" -w /src "$image" \
  sh -c 'git config --global --add safe.directory "*" && cmake --preset release-linux -DLLMGATE_VERSION="$VERSION" && cmake --build --preset release-linux'
tools/check_relocatable.sh "$root/build/release-linux/llmgate"
