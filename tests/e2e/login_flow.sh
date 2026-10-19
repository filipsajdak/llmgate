#!/bin/sh
set -eu
binary=${1:?usage: login_flow.sh <llmgate binary>}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
failures=0
started=$(date +%s)
check() { elapsed=$(($(date +%s) - started)); if "$@"; then echo "ok   [${elapsed}s] $label"; else echo "FAIL [${elapsed}s] $label"; failures=$((failures + 1)); fi; }
server=
trap '[ -z "$server" ] || kill "$server" 2>/dev/null || true; rm -rf "$work"' EXIT

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')

write_config() {
  cat > "$work/llmgate.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "upstream": { "base_url": "http://127.0.0.1:1" }$1
}
JSON
}

label="login without a token file in the configuration is refused with the configuration exit code"
write_config ""
if "$binary" --config "$work/llmgate.jsonc" login >/dev/null 2>"$work/err.txt"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -eq 3 ] && grep -q 'copilot.token_file' '$work/err.txt'"

label="login says GitHub is unreachable when nothing answers"
write_config ", \"copilot\": { \"token_file\": \"$work/token.json\", \"github_url\": \"http://127.0.0.1:1\" }"
if "$binary" --config "$work/llmgate.jsonc" login >/dev/null 2>"$work/err.txt"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -eq 1 ] && grep -q 'cannot reach GitHub' '$work/err.txt' && [ ! -e '$work/token.json' ]"

python3 - "$port" <<'PY' &
import http.server, json, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/login/device/code":
            body = {"device_code": "dc", "user_code": "ABCD-1234", "verification_uri": "http://example.test/device", "expires_in": 60, "interval": 0}
        else:
            body = {"access_token": "gho_e2e_token"}
        data = json.dumps(body).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
server=$!
for _ in $(seq 100); do
  python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', $port), 1).close()" 2>/dev/null && break
  sleep 0.1
done

label="login prints the code, waits for authorisation and stores the token"
write_config ", \"copilot\": { \"token_file\": \"$work/token.json\", \"github_url\": \"http://127.0.0.1:$port\" }"
"$binary" --config "$work/llmgate.jsonc" login > "$work/out.txt"
check sh -c "grep -q 'ABCD-1234' '$work/out.txt' && grep -q 'Logged in' '$work/out.txt' && grep -q 'gho_e2e_token' '$work/token.json'"

label="the stored token is private to the user"
check sh -c "[ \"\$(ls -l '$work/token.json' | cut -c1-10)\" = '-rw-------' ]"

label="an unknown argument prints the usage and exits with the usage code"
if "$binary" --nonsense >/dev/null 2>"$work/err.txt"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -eq 2 ] && grep -q 'unknown argument' '$work/err.txt' && grep -q '^usage: llmgate' '$work/err.txt'"

label="a missing configuration file is refused with the configuration exit code"
if "$binary" --config "$work/missing.jsonc" env >/dev/null 2>"$work/err.txt"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -eq 3 ] && grep -q 'cannot read' '$work/err.txt'"

echo "failures: $failures"
[ "$failures" -eq 0 ]
