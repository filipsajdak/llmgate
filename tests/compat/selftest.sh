#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 "$root/tests/compat/run.py" --backend scripted --binary "$binary" --out "$out/good"
if python3 "$root/tests/compat/run.py" --backend scripted --binary "$binary" --idle-stub --out "$out/idle" > "$out/idle.txt" 2>&1; then
  echo "FAIL a do-nothing stub passed every check: some check cannot fail"
  exit 1
fi
python3 - "$out/idle/scripted.json" <<'PY'
import json, sys
results = json.load(open(sys.argv[1]))["results"]
passed = sorted(name for name, row in results.items() if row["status"] == "PASS")
if passed:
    sys.exit("FAIL these scenarios passed with a do-nothing stub, so their check cannot fail: " + ", ".join(passed))
print("ok   every check fails when the work is not done")
PY
python3 - "$out/good/scripted.json" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
if report["betas"].get("compat-selftest-2026", 0) < 1:
    sys.exit("FAIL the beta census did not see the token the stub sent")
print("ok   the gateway's beta census saw the stub's token")
PY
spaced="$out/path with ' quote"
mkdir -p "$spaced/tests"
if command -v cygpath >/dev/null 2>&1; then spaced=$(cygpath -m "$spaced"); fi
cp -R "$root/tests/compat" "$root/tests/e2e" "$spaced/tests/"
python3 "$spaced/tests/compat/run.py" --backend scripted --binary "$binary" --out "$out/spaced" --scenario text-turn --scenario switch-mid-session
echo "ok   the harness runs from a path with a space and a quote"
