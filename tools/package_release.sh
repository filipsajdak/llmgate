#!/bin/sh
set -eu
bin=${1:?usage: package_release.sh BINARY OS ARCH VERSION OUTDIR}
os=${2:?}
arch=${3:?}
version=${4:?}
out=${5:?}
root=$(cd "$(dirname "$0")/.." && pwd)
name="llmgate-$version-$os-$arch"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name" "$out"
cp "$bin" "$stage/$name/llmgate"
cp "$root/LICENSE" "$root/README.md" "$stage/$name/"
tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
echo "$out/$name.tar.gz"
