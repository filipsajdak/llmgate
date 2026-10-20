#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
binary=${1:?usage: use_flow.sh <llmgate binary>}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
cleanup() {
  kill ${gateway_pid:-} ${a_pid:-} ${b_pid:-} 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

cat > "$work/upstream.py" <<'PY'
import fast_http, http.server, json, sys
name = sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))))
        body = json.dumps({"id": "msg_e2e", "type": "message", "role": "assistant", "model": request["model"],
                           "content": [{"type": "text", "text": "from-" + name + ":" + request["model"]}],
                           "stop_reason": "end_turn", "usage": {"input_tokens": 3, "output_tokens": 4}}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
server.daemon_threads = True
open(sys.argv[1], "w").write(str(server.server_address[1]))
server.serve_forever()
PY

start_upstream() {
  rm -f "$work/$1.port"
  python3 "$work/upstream.py" "$work/$1.port" "$1" &
  eval "$1_pid=$!"
  while [ ! -s "$work/$1.port" ]; do sleep 0.1; done
}
start_upstream a
start_upstream b
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
export E2E_TOKEN=t
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "claude": { "profile_dir": "$work/profile" },
  "providers": {
    "a": { "base_url": "http://127.0.0.1:$(cat "$work/a.port")", "static_token_env": "E2E_TOKEN", "models": { "claude-opus-5-5": "model-a" } },
    "b": { "base_url": "http://127.0.0.1:$(cat "$work/b.port")", "static_token_env": "E2E_TOKEN", "models": { "claude-opus-5-5": "model-b" } }
  }
}
JSON
config="$work/config.jsonc"
llmgate() { "$binary" --config "$config" "$@"; }

start_gateway() {
  : > "$work/gateway.log"
  "$binary" --config "$config" serve > "$work/gateway.log" 2>&1 &
  gateway_pid=$!
  while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done
}
stop_gateway() { kill "$gateway_pid"; wait "$gateway_pid" 2>/dev/null || true; }
reply() {
  curl -s -X POST "http://127.0.0.1:$port/anthropic/v1/messages" -H 'content-type: application/json' \
    -d '{"model":"claude-opus-5-5","max_tokens":8,"messages":[{"role":"user","content":"hi"}]}'
}
answered_by() { reply | tee -a "$work/replies.log" | grep -q "from-$1"; }

start_gateway
label="targets lists a target per provider model"
check sh -c "'$binary' --config '$config' targets | grep -q 'claude-model-b@b'"
label="before any remap the configured order answers (provider a first)"
check answered_by a
label="use opus <target> is accepted"
check llmgate use opus claude-model-b@b
label="the next reply comes from provider b"
check answered_by b
label="route reports the global remap"
check sh -c "'$binary' --config '$config' route claude-opus-5-5 | grep -q global_remap"
label="the overrides file is private"
check sh -c "[ \"\$(ls -l '$work/state/llmgate/overrides.json' | cut -c1-10)\" = '-rw-------' ]"
stop_gateway
start_gateway
label="a restarted gateway keeps the remap"
check answered_by b
label="use opus clear restores the configured order"
llmgate use opus clear >/dev/null
check answered_by a
label="a session remap needs the session to match"
check sh -c "'$binary' --config '$config' use opus claude-model-b@b --session s1 && '$binary' --config '$config' route claude-opus-5-5 --session s1 | grep -q session_remap && '$binary' --config '$config' route claude-opus-5-5 --session s2 | grep -q '\"config\"'"
label="the status line names the provider that answered the session"
curl -s -X POST "http://127.0.0.1:$port/anthropic/v1/messages" -H 'content-type: application/json' -H 'x-claude-code-session-id: sess-e2e' \
  -d '{"model":"claude-opus-5-5","max_tokens":8,"messages":[]}' > /dev/null
check sh -c "echo '{\"session_id\":\"sess-e2e\",\"model\":{\"display_name\":\"Opus\"},\"workspace\":{\"current_dir\":\"/w/p\"}}' | CLAUDE_CONFIG_DIR=/x/.claude-llmgate-siili '$binary' --config '$config' statusline | head -1 | grep -q 'Opus via a/model-a as siili'"
label="an unknown target is refused with a non-zero exit"
check sh -c "! '$binary' --config '$config' use opus claude-nope@b 2>/dev/null"

[ ! -s "$work/replies.log" ] || { echo "== replies:"; cat "$work/replies.log"; echo; }
echo "failures: $failures"
[ "$failures" -eq 0 ]
