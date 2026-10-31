#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${LLMGATE_BINARY:-$root/build/host/llmgate}
report_dir=${LLMGATE_REPORT_DIR:-$root/build/realrun}

if [ "${LLMGATE_REAL:-}" != 1 ]; then
  echo "refusing to run: this sends requests to a real Copilot service. Set LLMGATE_REAL=1 and LLMGATE_GHE_HOST=<your GitHub host>." >&2
  exit 2
fi
host=${LLMGATE_GHE_HOST:?set LLMGATE_GHE_HOST to the GitHub host, for example example.ghe.com}

case $host in
  *.ghe.com)
    github_url=${LLMGATE_GITHUB_URL:-https://$host}
    api_url=${LLMGATE_GITHUB_API_URL:-https://api.$host}
    copilot_base=${LLMGATE_COPILOT_BASE_URL:-https://copilot-api.$host} ;;
  *)
    github_url=${LLMGATE_GITHUB_URL:-https://$host}
    api_url=${LLMGATE_GITHUB_API_URL:-https://$host/api/v3}
    copilot_base=${LLMGATE_COPILOT_BASE_URL:-https://api.githubcopilot.com} ;;
esac

work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
chmod 700 "$work"
token_file=${LLMGATE_TOKEN_FILE:-$work/copilot-token.json}
case $(cd "$(dirname "$token_file")" 2>/dev/null && pwd)/ in
  "$root"/*) echo "refusing: LLMGATE_TOKEN_FILE must be outside the repository" >&2; exit 2 ;;
esac
provider_headers=""
if [ "${LLMGATE_COPILOT_HEADERS:-}" = 1 ]; then
  provider_headers='"headers": { "Editor-Version": "vscode/1.99.0", "Copilot-Integration-Id": "vscode-chat" },'
fi
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME" "$report_dir"
config="$work/llmgate.jsonc"
cleanup() {
  "$binary" --config "$config" stop >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$config" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "default": {
      "base_url": "$copilot_base",
      $provider_headers
      "copilot": { "token_file": "$token_file", "github_url": "$github_url", "api_url": "$api_url" },
      "models": { "claude-sonnet-5-5": "claude-sonnet-5.5", "claude-opus-5-5": "claude-opus-5.5", "claude-haiku-5-5": "claude-haiku-5.5" }
    }
  },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON

echo "== configuration"
"$binary" --config "$config" --check-config
if [ -s "$token_file" ]; then
  echo "== login: reusing the token in LLMGATE_TOKEN_FILE"
else
  echo "== login: authorise the code below in your browser"
  mkdir -p "$(dirname "$token_file")"
  "$binary" --config "$config" login
fi
"$binary" --config "$config" setup >/dev/null
echo "== doctor"
"$binary" --config "$config" doctor > "$work/doctor.txt" || true
cat "$work/doctor.txt"
"$binary" --config "$config" quota > "$work/quota.txt" 2>&1 || true

sample="$work/sample"
mkdir -p "$sample"
cat > "$sample/mathlib.py" <<'PY'
def add(a, b):
    return a - b
PY
cat > "$sample/test_mathlib.py" <<'PY'
from mathlib import add

assert add(2, 3) == 5, "add(2, 3) must be 5"
assert add(-1, 1) == 0, "add(-1, 1) must be 0"
print("all tests passed")
PY
if (cd "$sample" && python3 test_mathlib.py >/dev/null 2>&1); then
  echo "the sample must start broken" >&2
  exit 1
fi

echo "== one Claude Code turn with tool use through llmgate (edits in the sample directory only; Bash limited to the sample test)"
prompt="In this directory, read mathlib.py, fix the bug so that test_mathlib.py passes, and run python3 test_mathlib.py to confirm."
set +e
(cd "$sample" && "$binary" --config "$config" claude -p "$prompt" --model sonnet --no-session-persistence --output-format stream-json --verbose \
  --permission-mode acceptEdits --allowedTools 'Bash(python3 test_mathlib.py:*)' < /dev/null > "$work/claude.jsonl" 2> "$work/claude.err")
claude_status=$?
(cd "$sample" && python3 test_mathlib.py >/dev/null 2>&1)
sample_status=$?
set -e

if [ "$claude_status" -ne 0 ]; then
  echo "== claude failed (exit $claude_status); its stderr, first 600 bytes:"
  head -c 600 "$work/claude.err"
  echo
  echo "== gateway log, last lines:"
  tail -5 "$XDG_STATE_HOME/llmgate/gateway.log" 2>/dev/null || true
fi
env_json=$("$binary" --config "$config" env --json)
python3 - "$work" "$claude_status" "$sample_status" "$report_dir/report.md" "$env_json" "${LLMGATE_SAMPLE_MUST_PASS:-1}" <<'PY'
import collections
import json
import sys
import urllib.request

work, claude_status, sample_status, report_path, env_json, sample_must_pass = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], json.loads(sys.argv[5]), sys.argv[6] == "1"
base = env_json["ANTHROPIC_BASE_URL"].rsplit("/anthropic", 1)[0]
token = env_json.get("ANTHROPIC_AUTH_TOKEN", "")
request = urllib.request.Request(base + "/llmgate/stats", headers={"x-api-key": token, "authorization": "Bearer " + token})
try:
    stats = json.load(urllib.request.urlopen(request, timeout=10))
except OSError as error:
    stats = {"error": type(error).__name__}

tools = collections.Counter()
turns = results = errors = refused = 0
error_text = ""
for line in open(f"{work}/claude.jsonl"):
    try:
        event = json.loads(line)
    except ValueError:
        continue
    if event.get("type") == "assistant":
        turns += 1
        for block in event["message"].get("content", []):
            if block.get("type") == "tool_use":
                tools[block["name"]] += 1
    if event.get("type") == "user":
        for block in event["message"].get("content", []) if isinstance(event["message"].get("content"), list) else []:
            if block.get("type") == "tool_result" and block.get("is_error"):
                refused += 1
    if event.get("type") == "result":
        results += 1
        errors += bool(event.get("is_error"))
        if event.get("is_error") and not error_text:
            error_text = str(event.get("result", ""))

ledger = []
try:
    ledger = [json.loads(line) for line in open(f"{work}/usage.jsonl")]
except OSError:
    pass
doctor = collections.Counter(word for line in open(f"{work}/doctor.txt") for word in ("PASS", "WARN", "FAIL") if line.startswith(word))
repairs = {kind: count for kind, count in (stats.get("repairs") or {}).items() if count}
provider = (stats.get("providers") or {}).get("default", {})
served = sorted({(row["requested"], row["model"]) for row in ledger})
statuses = collections.Counter(row["status"] for row in ledger)

passed = claude_status == 0 and (sample_status == 0 or not sample_must_pass) and sum(tools.values()) > 0 and not stats.get("upstream_failures") and errors == 0 and results == 1
import re
error_text = re.sub(r"(gho_|ghu_|ghs_|tid=)[A-Za-z0-9_=;:.-]+", r"\1<redacted>", " ".join(error_text.split()))[:400]
lines = [
    "# Real run report",
    "",
    f"- result: {'PASS' if passed else 'FAIL'}",
    f"- claude exit status: {claude_status}; result events: {results}; error results: {errors}; assistant turns: {turns}",
    f"- tool calls seen: {dict(tools) or 'none'}; failed or refused by the permission scope: {refused}",
    f"- sample project fixed (its test passes afterwards): {'yes' if sample_status == 0 else 'no'}",
    f"- gateway: requests {stats.get('requests')}, streamed {stats.get('streamed')}, upstream failures {stats.get('upstream_failures')}, timeouts {stats.get('upstream_timeouts')}, "
    f"connections opened {stats.get('connections_opened')}, reused {stats.get('connections_reused')}",
    f"- stream repairs: {repairs or 'none'}; first example of each: {stats.get('repair_examples') or 'none'}",
    f"- provider default: {provider}",
    f"- ledger lines {len(ledger)}; statuses {dict(statuses)}; input {sum(r['input'] for r in ledger)}, output {sum(r['output'] for r in ledger)}, cache read {sum(r['cache_read'] for r in ledger)}, cache write {sum(r['cache_creation'] for r in ledger)}",
    f"- requested model -> served model: {served}",
    f"- doctor: {dict(doctor)}",
] + ([f"- error from the turn (first 400 characters, tokens redacted): {error_text}"] if error_text else [])
open(report_path, "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(0 if passed else 1)
PY
