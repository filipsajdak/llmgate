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
if [ "$os" = windows ]; then
  cp "$bin" "$stage/$name/llmgate.exe"
else
  cp "$bin" "$stage/$name/llmgate"
fi
cp "$root/LICENSE" "$root/README.md" "$stage/$name/"
if [ "$os" = windows ]; then
  python3 -I -c 'import pathlib, sys, zipfile
stage, name, target = sys.argv[1:]
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(pathlib.Path(stage, name).iterdir()):
        archive.write(path, f"{name}/{path.name}")' "$stage" "$name" "$out/$name.zip"
  echo "$out/$name.zip"
else
  tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
  echo "$out/$name.tar.gz"
fi
