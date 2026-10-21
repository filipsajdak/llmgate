#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:?usage: mod_flow.sh <llmgate binary>}
command -v claude > /dev/null 2>&1 || { echo "skip: claude is not installed"; exit 77; }
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
trap 'kill ${a_pid:-} 2>/dev/null || true; wait 2>/dev/null || true; "$binary" --config "$work/config.jsonc" stop > /dev/null 2>&1 || true; rm -rf "$work"' EXIT

cat > "$work/upstream.py" <<'PY'
import http.server, json, sys
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get("content-length", 0)))
        body = json.dumps({"id": "msg", "type": "message", "role": "assistant", "model": "m", "content": [{"type": "text", "text": "ok"}], "stop_reason": "end_turn", "usage": {"input_tokens": 1, "output_tokens": 1}}).encode()
        self.send_response(200); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(server.server_address[1]))
server.serve_forever()
PY
python3 "$work/upstream.py" "$work/up.port" &
a_pid=$!
while [ ! -s "$work/up.port" ]; do sleep 0.1; done
up="http://127.0.0.1:$(cat "$work/up.port")"

cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$(free_port)",
  "providers": {
    "alpha": { "base_url": "$up", "static_token_env": "E2E_TOKEN", "models": { "claude-sonnet-5-5": "m-alpha" }, "offer": ["m-alpha"] },
    "beta": { "base_url": "$up", "static_token_env": "E2E_TOKEN", "offer": ["m-beta"] }
  },
  "routes": { "sonnet": ["alpha"] },
  "claude": { "profile_dir": "$work/profile" }
}
JSON
export E2E_TOKEN=e2e-token

label="the mod validates and its own tests pass"
check sh -c "claude plugin validate '$root/mods/llmgate' > '$work/validate.out' 2>&1 && (cd '$root/mods/llmgate' && claude plugin test > '$work/test.out' 2>&1) && grep -q ' 0 fail' '$work/test.out'"

export LLMGATE_BIN="$binary" LLMGATE_CONFIG="$work/config.jsonc"
"$binary" --config "$work/config.jsonc" claude -p "/llmgate" --plugin-dir "$root/mods/llmgate" < /dev/null > "$work/command.out" 2>&1 || true
label="the command, run against a real gateway, reports the route of every role"
check sh -c "grep -q 'sonnet -> alpha' '$work/command.out' && grep -q 'opus' '$work/command.out' && grep -q 'haiku' '$work/command.out'"

"$binary" --config "$work/config.jsonc" use sonnet claude-m-beta@beta > "$work/use.out" 2>&1 || true
"$binary" --config "$work/config.jsonc" claude -p "/llmgate" --plugin-dir "$root/mods/llmgate" < /dev/null > "$work/after.out" 2>&1 || true
label="after llmgate use the command reports the new route"
check sh -c "grep -q 'sonnet -> beta' '$work/after.out'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; cat "$work/command.out" | head -5; exit 1; }
