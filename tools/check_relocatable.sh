#!/bin/sh
set -eu
bin=${1:?usage: check_relocatable.sh BINARY}
case "$(uname -s)" in
  Darwin) others=$(otool -L "$bin" | tail -n +2 | awk '{print $1}' | grep -v '^/usr/lib/' || true) ;;
  Linux)
    listing=$(ldd "$bin" 2>&1 || true)
    case "$listing" in
      *"not a dynamic executable"*|*"statically linked"*) others= ;;
      *) others=$listing ;;
    esac ;;
  *) echo "check_relocatable: unsupported system $(uname -s)" >&2; exit 2 ;;
esac
if [ -n "$others" ]; then
  echo "check_relocatable: $bin depends on libraries outside the system:" >&2
  echo "$others" >&2
  exit 1
fi
echo "check_relocatable: $bin has no dependencies outside the system"
