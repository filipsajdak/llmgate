#!/bin/sh
set -eu
binary=${1:?usage: setup_flow.sh <llmgate binary>}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
trap '"$binary" --config "$work/llmgate.jsonc" stop >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/llmgate.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "upstream": { "base_url": "http://127.0.0.1:1" },
  "claude": { "profile_dir": "$work/profile", "context_tokens": 150000, "sonnet": "claude-sonnet-5-5" }
}
JSON
cat > "$work/claude" <<'STUB'
#!/bin/sh
{ echo "ARGS: $*"; env | grep -E '^(ANTHROPIC_|CLAUDE_|DISABLE_)' | sort; } > "$STUB_OUTPUT"
STUB
chmod +x "$work/claude"
export STUB_OUTPUT="$work/claude.out" LLMGATE_CLAUDE="$work/claude"
config="$work/llmgate.jsonc"

label="setup --dry-run writes nothing"
out=$("$binary" --config "$config" setup --dry-run); [ ! -e "$work/profile/settings.json" ] && echo "$out" | grep -q "would write"
check test $? -eq 0

label="setup creates the profile settings"
"$binary" --config "$config" setup >/dev/null
check grep -q 'Read(\*\*/.env)' "$work/profile/settings.json"

label="settings carry effort and a status line command"
check grep -q '"effortLevel": "medium"' "$work/profile/settings.json"
label="statusLine command names this binary"
check grep -q 'statusline' "$work/profile/settings.json"

label="setup twice leaves the file byte-identical"
cp "$work/profile/settings.json" "$work/first.json"
"$binary" --config "$config" setup | grep -q unchanged
check cmp -s "$work/first.json" "$work/profile/settings.json"

label="a user's own settings survive another setup"
python3 - "$work/profile/settings.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p)); d["theme"] = "dark"; d["effortLevel"] = "high"; json.dump(d, open(p, "w"))
PY
"$binary" --config "$config" setup >/dev/null
check sh -c "grep -q '\"theme\": \"dark\"' '$work/profile/settings.json' && grep -q '\"effortLevel\": \"high\"' '$work/profile/settings.json'"

label="setup refuses settings that are not plain JSON and changes nothing"
cp "$work/profile/settings.json" "$work/backup.json"; echo '{ // comment' > "$work/profile/settings.json"
if "$binary" --config "$config" setup >/dev/null 2>"$work/err.txt"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'not plain JSON' '$work/err.txt' && grep -q comment '$work/profile/settings.json'"
cp "$work/backup.json" "$work/profile/settings.json"

label="env prints exports with the gateway url and models"
"$binary" --config "$config" env > "$work/env.out"
check sh -c "grep -q \"export ANTHROPIC_BASE_URL='http://127.0.0.1:$port/anthropic'\" '$work/env.out' && grep -q \"CLAUDE_CODE_MAX_CONTEXT_TOKENS='150000'\" '$work/env.out'"

label="env --vscode prints the variables as settings"
check sh -c "'$binary' --config '$config' env --vscode | grep -q 'claudeCode.disableLoginPrompt'"

label="setup --vscode FILE merges into an existing settings file"
echo '{"editor.fontSize": 14}' > "$work/vscode.json"
"$binary" --config "$config" setup --vscode "$work/vscode.json" >/dev/null
check sh -c "grep -q 'editor.fontSize' '$work/vscode.json' && grep -q 'ANTHROPIC_BASE_URL' '$work/vscode.json'"

label="status says the gateway is down before anything started it"
check sh -c "! '$binary' --config '$config' status >/dev/null 2>&1"

label="claude starts the gateway on demand and runs claude with the right environment"
"$binary" --config "$config" claude -p "hello there" --model sonnet
check grep -q "ARGS: -p hello there --model sonnet" "$work/claude.out"
label="claude got the base url, the sonnet slot and the profile dir"
check sh -c "grep -q 'ANTHROPIC_BASE_URL=http://127.0.0.1:$port/anthropic' '$work/claude.out' && grep -q 'ANTHROPIC_DEFAULT_SONNET_MODEL=claude-sonnet-5-5' '$work/claude.out' && grep -q 'CLAUDE_CONFIG_DIR=$work/profile' '$work/claude.out'"
label="telemetry is off in the launched environment"
check grep -q "DISABLE_TELEMETRY=1" "$work/claude.out"
label="the gateway is now up"
check sh -c "'$binary' --config '$config' status | grep -q 'gateway: up'"

label="a second claude launch reuses the same gateway process"
first_pid=$(cat "$work/state/llmgate/gateway.pid")
"$binary" --config "$config" claude --again
check sh -c "[ \"$first_pid\" = \"\$(cat '$work/state/llmgate/gateway.pid')\" ] && grep -q 'ARGS: --again' '$work/claude.out'"

label="the status line draws two lines with the model, the context bar and the gateway"
line=$(printf '{"model":{"display_name":"Opus"},"workspace":{"current_dir":"%s"},"context_window":{"used_percentage":55}}' "$work" | "$binary" --config "$config" statusline)
check sh -c "echo '$line' | grep -q 'llmgate\] Opus' && echo '$line' | grep -q 'ctx \[#####-----\] 55%' && echo '$line' | grep -q 'gateway ok'"

label="the status line still draws something when the gateway is unreachable"
"$binary" --config "$config" stop >/dev/null
sleep 0.5
line=$(printf '{"model":{"display_name":"Opus"}}' | "$binary" --config "$config" statusline)
check sh -c "echo '$line' | grep -q 'gateway DOWN'"

label="stop reports nothing is recorded the second time"
check sh -c "! '$binary' --config '$config' stop >/dev/null 2>&1"

label="a squatter on the port is refused and receives nothing"
python3 - "$port" "$work/squat.log" <<'PY' &
import http.server, sys
log = open(sys.argv[2], "w")
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        log.write(self.path + " " + str(dict(self.headers)) + "\n"); log.flush()
        body = b'{"status":"ok"}'; self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
squatter=$!
sleep 1
if "$binary" --config "$config" claude --never 2>"$work/err3.txt"; then rc=0; else rc=$?; fi
kill $squatter 2>/dev/null || true
check sh -c "[ $rc -ne 0 ] && grep -q 'not this user' '$work/err3.txt' && ! grep -q 'x-api-key\\|authorization' '$work/squat.log'"

label="claude fails clearly when no claude command exists"
if LLMGATE_CLAUDE=/nonexistent/claude "$binary" --config "$config" claude 2>"$work/err2.txt"; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'not found' '$work/err2.txt'"

echo "failures: $failures"
[ "$failures" -eq 0 ]
