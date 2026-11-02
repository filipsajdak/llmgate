#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
failures=0
started=$(date +%s)
check() { elapsed=$(($(date +%s) - started)); if "$@"; then echo "ok   [${elapsed}s] $label"; else echo "FAIL [${elapsed}s] $label"; failures=$((failures + 1)); fi; }
trap 'kill $upstream_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

FAKE_REQUIRE_AUTHORIZATION="Bearer client-login" python3 "$root/tests/e2e/fake_anthropic.py" "$work/upstream.port" "$work/upstream.log" &
upstream_pid=$!
while [ ! -s "$work/upstream.port" ]; do sleep 0.1; done
upstream_port=$(cat "$work/upstream.port")

cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:0",
  "providers": { "anthropic": { "base_url": "http://127.0.0.1:$upstream_port", "forward_credentials": true } },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
"$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
gateway_pid=$!
while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done
port=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$work/gateway.log")

post() {
  python3 - "$port" "$1" <<'PY'
import http.client, json, sys
port, authorization = int(sys.argv[1]), sys.argv[2]
headers = {"content-type": "application/json", "anthropic-version": "2023-06-01", "anthropic-beta": "oauth-2025-04-20,advisor-tool-2026-03-01", "anthropic-workspace-id": "ws-1"}
if authorization:
    headers["Authorization"] = authorization
connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
connection.request("POST", "/anthropic/v1/messages", json.dumps({"model": "claude-sonnet-5-5", "max_tokens": 8, "messages": [{"role": "user", "content": "hi"}]}), headers)
reply = connection.getresponse()
body = reply.read().decode()
print(reply.status, "authentication_error" in body)
PY
}

label="the client's login reaches the Anthropic provider and the request is answered"
answer=$(post 'Bearer client-login')
check test "$answer" = "200 False"
label="a wrong login is refused by the upstream and its error body reaches the client unchanged"
answer=$(post 'Bearer someone-else')
check test "$answer" = "401 True"
label="no login at all is refused the same way: the gateway adds no credential of its own"
answer=$(post '')
check test "$answer" = "401 True"
label="the upstream saw the client's Authorization, the whole anthropic-beta value and the open-list header"
check python3 - "$work/upstream.log" <<'PY'
import json, sys
seen = [json.loads(line) for line in open(sys.argv[1])]
good = [e for e in seen if not e.get("rejected")]
h = good[0]["headers"] if good else {}
sys.exit(0 if h.get("authorization") == "Bearer client-login" and h.get("anthropic-beta") == "oauth-2025-04-20,advisor-tool-2026-03-01" and h.get("anthropic-workspace-id") == "ws-1" else 1)
PY

cat > "$work/keyed.jsonc" <<JSON
{
  "listen": "127.0.0.1:0",
  "client_key_env": "LLMGATE_CLIENT_KEY",
  "providers": { "anthropic": { "base_url": "http://127.0.0.1:$upstream_port", "forward_credentials": true } }
}
JSON
label="--check-config refuses a forwarding provider together with client_key_env"
if LLMGATE_CLIENT_KEY=k "$binary" --config "$work/keyed.jsonc" --check-config > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'anthropic' '$work/out.txt' && grep -q 'client_key_env' '$work/out.txt'"
label="serving a forwarding provider with a client key is refused"
if LLMGATE_CLIENT_KEY=k "$binary" --config "$work/keyed.jsonc" > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'client_key_env' '$work/out.txt'"

echo "failures: $failures"
[ "$failures" -eq 0 ]
