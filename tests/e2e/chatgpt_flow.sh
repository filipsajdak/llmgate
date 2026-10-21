#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
export LLMGATE_NO_BROWSER=1
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
trap 'kill $auth_pid $upstream_pid $login_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

FAKE_AUTH_EXPIRES_IN=${EXPIRES_IN:-3600} python3 "$root/tests/e2e/fake_openai_auth.py" "$work/auth.port" "$work/auth.log" &
auth_pid=$!
FAKE_REQUIRE_AUTHORIZATION="" python3 "$root/tests/e2e/fake_responses.py" "$work/up.port" "$work/up.log" text &
upstream_pid=$!
while [ ! -s "$work/auth.port" ] || [ ! -s "$work/up.port" ]; do sleep 0.1; done

gateway_port=$(free_port)
callback_port=$(free_port)
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$gateway_port",
  "providers": {
    "codex": {
      "base_url": "http://127.0.0.1:$(cat "$work/up.port")",
      "protocol": "openai_responses",
      "chatgpt": { "token_file": "$work/chatgpt-token.json", "issuer": "http://127.0.0.1:$(cat "$work/auth.port")", "callback_port": $callback_port },
      "models": { "claude-sonnet-5-5": "gpt-fake" }
    }
  }
}
JSON

"$binary" --config "$work/config.jsonc" login --provider codex > "$work/login.out" 2>&1 &
login_pid=$!
for _ in $(seq 1 100); do grep -q "authorize?" "$work/login.out" && break; sleep 0.1; done
url=$(grep -o 'http://127.0.0.1:[0-9]*/api/accounts/authorize[^ ]*' "$work/login.out" | head -1)
label="the login prints an authorize URL with the documented parameters"
check sh -c "echo '$url' | grep -q 'code_challenge_method=S256' && echo '$url' | grep -q 'client_id=dynamic_agent_client' && echo '$url' | grep -q 'agent_name_hint=llmgate'"
curl -sL -o /dev/null "$url"
wait "$login_pid" && login_status=0 || login_status=$?
label="the login finishes with status 0 and says who signed in"
check sh -c "[ $login_status -eq 0 ] && grep -q 'Signed in as e2e-user' '$work/login.out'"
label="the token file is private and holds the issued client id, host id and rotated tokens"
check sh -c "[ \"\$(ls -l '$work/chatgpt-token.json' | cut -c1-10)\" = '-rw-------' ] && grep -q issued_e2e '$work/chatgpt-token.json' && grep -q 'urn:uuid:' '$work/chatgpt-token.json' && grep -q refresh-1 '$work/chatgpt-token.json'"
label="no token appears in the login output"
check sh -c "! grep -q 'access-1\\|refresh-1' '$work/login.out'"

doctor_says() { "$binary" --config "$work/config.jsonc" doctor --offline 2>&1 | grep -A1 'chatgpt-login' | grep -q "$1"; }
label="doctor reports the saved sign-in as passing"
check doctor_says PASS
cp "$work/chatgpt-token.json" "$work/token.backup"
chmod 644 "$work/chatgpt-token.json"
label="doctor fails a token file other users can read"
check doctor_says FAIL
chmod 600 "$work/chatgpt-token.json"
python3 - "$work/chatgpt-token.json" <<'PY'
import json, sys
path = sys.argv[1]
tokens = json.load(open(path))
tokens["scope"] = "openid offline_access"
json.dump(tokens, open(path, "w"))
PY
label="doctor fails a sign-in that does not grant plan use"
check doctor_says 'plan use'
cp "$work/token.backup" "$work/chatgpt-token.json"
chmod 600 "$work/chatgpt-token.json"
rm "$work/chatgpt-token.json"
label="doctor fails a missing sign-in and names the command"
check doctor_says 'llmgate login --provider codex'
cp "$work/token.backup" "$work/chatgpt-token.json"
chmod 600 "$work/chatgpt-token.json"

start_gateway() {
  : > "$work/gateway.log"
  "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
  gateway_pid=$!
  while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done
}
answered() {
  code=$(curl -s -o "$work/reply.out" -w '%{http_code}' -X POST "http://127.0.0.1:$gateway_port/anthropic/v1/messages" -H 'content-type: application/json' \
    -d '{"model":"claude-sonnet-5-5","max_tokens":8,"stream":true,"messages":[{"role":"user","content":"hi"}]}')
  [ "$code" = 200 ] && grep -q 'event: message_stop' "$work/reply.out" && grep -q 'responses upstream' "$work/reply.out"
}
saw_token() { grep -q "Bearer $1" "$work/up.log"; }
file_has() { grep -q "$1" "$work/chatgpt-token.json"; }

start_gateway
label="a request through the gateway is answered"
check answered
label="the upstream saw the plan access token from the sign-in"
check saw_token access-1

kill "$gateway_pid"; wait "$gateway_pid" 2>/dev/null || true
python3 - "$work/chatgpt-token.json" <<'PY'
import json, sys
path = sys.argv[1]
tokens = json.load(open(path))
tokens["expires_at"] = 1
json.dump(tokens, open(path, "w"))
PY
start_gateway
label="an expired stored token is refreshed with the refresh token and the upstream sees the new access token"
check answered
check saw_token access-2
label="the rotated refresh token replaced the old one in the private file"
check file_has refresh-2
label="the auth server saw one code exchange and one refresh"
check sh -c "[ \$(grep -c '\"grant\": \"authorization_code\"' '$work/auth.log') -eq 1 ] && [ \$(grep -c '\"grant\": \"refresh_token\"' '$work/auth.log') -eq 1 ]"

if [ "$failures" -ne 0 ]; then
  echo "--- login output"; sed 's/code_challenge=[^&]*/code_challenge=<cut>/' "$work/login.out" | cut -c1-300
  echo "--- auth log"; cat "$work/auth.log" 2>/dev/null || true
  echo "--- gateway reply"; head -c 400 "$work/reply.out" 2>/dev/null; echo; echo "--- gateway log"; tail -5 "$work/gateway.log" | cut -c1-200
  echo "--- upstream log (paths only)"; python3 -c 'import json,sys; [print(json.loads(l)["path"], json.loads(l)["headers"].get("authorization","")[:20]) for l in open(sys.argv[1])]' "$work/up.log" 2>/dev/null || true
fi
echo "failures: $failures"
[ "$failures" -eq 0 ]
