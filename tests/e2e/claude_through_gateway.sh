#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${LLMGATE_BINARY:-$root/build/host/llmgate}
defect=${1:-none}
prompt="Say hello"
tool_flags=""
expected="Hello from the fake upstream"
if [ "$defect" = tool ]; then
  prompt="Run the command echo tool-ran with the Bash tool, then say it finished."
  tool_flags="--allowedTools Bash"
  expected="Tool finished: all good"
fi
work=$(mktemp -d)
trap 'kill $upstream_pid $gateway_pid 2>/dev/null || true; rm -rf "$work"' EXIT

python3 "$root/tests/e2e/fake_anthropic.py" "$work/upstream.port" "$work/upstream.log" "$defect" &
upstream_pid=$!
while [ ! -s "$work/upstream.port" ]; do sleep 0.1; done
upstream_port=$(cat "$work/upstream.port")

cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:0",
  "upstream": { "base_url": "http://127.0.0.1:$upstream_port", "messages_path": "/v1/messages" },
  "models": { "sonnet": "fake-sonnet-upstream", "haiku": "fake-haiku-upstream" },
  "static_token_env": "LLMGATE_TEST_TOKEN",
  "client_key_env": "LLMGATE_CLIENT_KEY"
}
JSON

LLMGATE_TEST_TOKEN=fake-upstream-token LLMGATE_CLIENT_KEY=local-client-key "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
gateway_pid=$!
while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done
gateway_port=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$work/gateway.log")

echo "gateway :$gateway_port -> fake upstream :$upstream_port (defect: $defect)"
ANTHROPIC_BASE_URL="http://127.0.0.1:$gateway_port/anthropic" ANTHROPIC_API_KEY=local-client-key \
  CLAUDE_CONFIG_DIR="$work/claude" DISABLE_TELEMETRY=1 CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1 DISABLE_AUTOUPDATER=1 \
  claude -p "$prompt" --model sonnet --no-session-persistence --output-format text $tool_flags > "$work/claude.out" 2> "$work/claude.err" || true

echo "--- claude printed:"; cat "$work/claude.out"
echo "--- claude stderr:"; head -5 "$work/claude.err"
echo "--- requests the upstream received:"
python3 - "$work/upstream.log" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    e = json.loads(line)
    body = e["body"]
    summary = body[:90].replace("\n", " ")
    h = e["headers"]
    print(f'{e["method"]} {e["path"]} auth={h.get("authorization")} x-api-key={h.get("x-api-key")} beta={h.get("anthropic-beta")} :: {summary}')
PY
echo "--- access control:"
echo "no key:        $(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$gateway_port/anthropic/v1/messages" -d '{}')"
echo "spoofed Host:  $(curl -s -o /dev/null -w '%{http_code}' -H 'Host: evil.example' -H 'x-api-key: local-client-key' "http://127.0.0.1:$gateway_port/anthropic/v1/models")"
echo "right key:     $(curl -s -o /dev/null -w '%{http_code}' -H 'x-api-key: local-client-key' "http://127.0.0.1:$gateway_port/anthropic/v1/models")"
echo "--- gateway stats:"; curl -s -H 'x-api-key: local-client-key' "http://127.0.0.1:$gateway_port/llmgate/stats"; echo
grep -q "$expected" "$work/claude.out"
if [ "$defect" = tool ]; then
  python3 - "$work/upstream.log" <<'PY'
import json, sys
entries = [json.loads(line) for line in open(sys.argv[1])]
answered = [e for e in entries if e.get("has_tool_result")]
print(f"requests that answered a tool call: {len(answered)}")
sys.exit(0 if answered else 1)
PY
fi
