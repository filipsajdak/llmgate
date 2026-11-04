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
  MINGW*|MSYS*|CYGWIN*)
    system_dlls='^(kernel32|kernelbase|advapi32|ws2_32|crypt32|bcrypt|ncrypt|user32|shell32|ole32|oleaut32|ntdll|ucrtbase|msvcrt|secur32|iphlpapi|userenv|shlwapi|api-ms-win-[a-z0-9-]+)\.dll$'
    others=$(objdump -p "$bin" | awk '/DLL Name:/ { print tolower($3) }' | grep -Ev "$system_dlls" || true) ;;
  *) echo "check_relocatable: unsupported system $(uname -s)" >&2; exit 2 ;;
esac
if [ -n "$others" ]; then
  echo "check_relocatable: $bin depends on libraries outside the system:" >&2
  echo "$others" >&2
  exit 1
fi
echo "check_relocatable: $bin has no dependencies outside the system"
