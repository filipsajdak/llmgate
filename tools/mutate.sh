#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
venv=build/mutate-venv
if ! command -v mutate >/dev/null 2>&1; then
  [ -x "$venv/bin/mutate" ] || { python3 -m venv "$venv" && "$venv/bin/pip" install -q universalmutator; }
  PATH="$root/$venv/bin:$PATH"
  export PATH
fi
exec python3 tools/mutate.py "$@"
