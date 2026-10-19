#!/bin/sh
set -eu
binary=${1:?usage: doctor_flow.sh <llmgate binary>}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
cleanup() {
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "== the script ended with status $status; the gateway log:"
    tail -n 40 "$work/state/llmgate/gateway.log" 2>/dev/null || echo "(no gateway log)"
  fi
  kill $(cat "$work/state/llmgate/gateway.pid" 2>/dev/null) 2>/dev/null || true
  kill ${github_pid:-} ${squatter_pid:-} 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 143' TERM INT

free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
port=$(free_port)
github_port=$(free_port)
config="$work/llmgate.jsonc"
secret=gho_fake_secret_value

cat > "$work/github.py" <<'PY'
import http.server, json, sys, time

MODE = sys.argv[2]

def mode():
    return open(MODE).read().strip()

LOW = {"chat": (200, 4, 2), "completions": (2000, 1983, 99)}
OK = {"chat": (200, 196, 98), "completions": (2000, 1983, 99)}

def user_body(table):
    snaps = {name: {"entitlement": e, "remaining": r, "percent_remaining": float(p), "quota_remaining": float(r), "unlimited": False,
                    "has_quota": True, "overage_permitted": False, "quota_id": name} for name, (e, r, p) in table.items()}
    snaps["premium_interactions"] = {"entitlement": 0, "remaining": 0, "percent_remaining": 0.0, "unlimited": False, "has_quota": False}
    return {"login": "someone", "copilot_plan": "individual", "access_type_sku": "free_limited_copilot", "quota_reset_date": "2026-11-01",
            "quota_snapshots": snaps, "endpoints": {"api": "http://127.0.0.1:1"}}

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        state = mode()
        if state == "unauthorized":
            return self.reply(401, {"message": "bad credentials"})
        if self.path == "/copilot_internal/v2/token":
            return self.reply(200, {"token": "jwt-e2e", "expires_at": int(time.time()) + 1800, "refresh_in": 1500, "endpoints": {"api": "http://127.0.0.1:1"}})
        if self.path == "/copilot_internal/user":
            if state == "error":
                return self.reply(500, {"message": "boom"})
            return self.reply(200, user_body(LOW if state == "low" else OK))
        self.reply(404, {})

server = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H)
server.serve_forever()
PY

echo ok > "$work/github.mode"
python3 "$work/github.py" "$github_port" "$work/github.mode" &
github_pid=$!
while ! python3 -c "import socket; socket.create_connection(('127.0.0.1',$github_port),1)" 2>/dev/null; do sleep 0.1; done

mkdir -p "$work/config/llmgate"
printf '{"oauth_token":"%s"}\n' "$secret" > "$work/config/llmgate/token.json"
chmod 600 "$work/config/llmgate/token.json"
cat > "$config" <<JSON
{
  "listen": "127.0.0.1:$port",
  "upstream": { "base_url": "http://127.0.0.1:1" },
  "claude": { "profile_dir": "$work/profile" },
  "usage": { "ledger_file": "$work/usage.jsonl", "budget": { "daily_tokens": 1000000, "monthly_tokens": 20000000 } },
  "copilot": { "token_file": "$work/config/llmgate/token.json", "github_url": "http://127.0.0.1:$github_port", "api_url": "http://127.0.0.1:$github_port" }
}
JSON
printf '#!/bin/sh\nexit 0\n' > "$work/claude"
chmod +x "$work/claude"
export LLMGATE_CLAUDE="$work/claude"

doctor() { if "$binary" --config "$config" doctor "$@" > "$work/doctor.out" 2>&1; then rc=0; else rc=$?; fi; }
has() { grep -q "$1" "$work/doctor.out"; }

doctor --offline
label="before setup the profile is a warning with the remedy, and the run still exits 0"
check sh -c "[ $rc -eq 0 ] && grep -q 'WARN  profile' '$work/doctor.out' && grep -q 'fix: run llmgate setup' '$work/doctor.out' && grep -q 'network checks were skipped' '$work/doctor.out'"

"$binary" --config "$config" setup >/dev/null
doctor --offline
label="after setup every offline check passes and the secret is never printed"
check sh -c "[ $rc -eq 0 ] && ! grep -q 'WARN\\|FAIL' '$work/doctor.out' && ! grep -q '$secret' '$work/doctor.out'"

doctor
label="online: the login is accepted and the quota is healthy"
check sh -c "[ $rc -eq 0 ] && grep -q 'PASS  copilot-exchange' '$work/doctor.out' && grep -q 'PASS  quota' '$work/doctor.out' && ! grep -q 'jwt-e2e\\|$secret' '$work/doctor.out'"

doctor --json
label="--json is machine readable and says ok"
check sh -c "python3 -c \"import json; d = json.load(open('$work/doctor.out')); assert d['ok'] is True and {'config', 'quota', 'copilot-exchange'} <= {f['check'] for f in d['findings']}\""

"$binary" --config "$config" quota > "$work/quota.out"
label="quota shows the plan, remaining of total, the reset date and a plan without premium requests"
check sh -c "grep -q 'plan: individual (free_limited_copilot)' '$work/quota.out' && grep -q 'chat: 196 of 200 left (98%)' '$work/quota.out' && grep -q 'resets: 2026-11-01' '$work/quota.out' && grep -q 'premium requests: not included in this plan' '$work/quota.out'"

label="quota --json carries the numbers"
check sh -c "'$binary' --config '$config' quota --json | python3 -c \"import json, sys; d = json.load(sys.stdin); q = {x['name']: x for x in d['quotas']}; assert q['chat']['remaining'] == 196 and q['chat']['entitlement'] == 200 and d['reset_date'] == '2026-11-01'\""

main_config="$config"
config="$work/providers.jsonc"
cat > "$config" <<JSON
{
  "listen": "127.0.0.1:$port",
  "claude": { "profile_dir": "$work/profile" },
  "usage": { "ledger_file": "$work/usage.jsonl" },
  "providers": { "default": { "base_url": "http://127.0.0.1:1",
    "copilot": { "token_file": "$work/config/llmgate/token.json", "github_url": "http://127.0.0.1:$github_port", "api_url": "http://127.0.0.1:$github_port" } } }
}
JSON
doctor
label="a Copilot login under providers is found: the token file and the exchange are checked and no credential is reported missing"
check sh -c "[ $rc -eq 0 ] && grep -q 'PASS  copilot-login' '$work/doctor.out' && grep -q 'PASS  copilot-exchange' '$work/doctor.out' && ! grep -q 'no upstream credential' '$work/doctor.out'"
"$binary" --config "$config" quota > "$work/quota.out" 2>&1 || true
label="quota reads the Copilot login under providers"
check sh -c "grep -q 'plan: individual' '$work/quota.out'"
config="$main_config"

echo low > "$work/github.mode"
doctor
label="a low quota is a WARN with a remedy and does not fail the run"
check sh -c "[ $rc -eq 0 ] && grep -q 'WARN  quota' '$work/doctor.out' && grep -q 'chat 2% left' '$work/doctor.out' && grep -q 'fix:' '$work/doctor.out'"

echo error > "$work/github.mode"
if "$binary" --config "$config" quota > "$work/quota.out" 2> "$work/quota.err"; then rc=0; else rc=$?; fi
label="an unavailable quota endpoint gives a clear message and a non-zero exit"
check sh -c "[ $rc -ne 0 ] && grep -q 'quota is unavailable' '$work/quota.err'"
doctor
label="and doctor degrades to a warning"
check sh -c "[ $rc -eq 0 ] && grep -q 'WARN  quota' '$work/doctor.out'"

echo unauthorized > "$work/github.mode"
doctor
label="a login GitHub rejects fails the run and says to log in again"
check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  copilot-exchange' '$work/doctor.out' && grep -q 'fix: run llmgate login' '$work/doctor.out'"
echo ok > "$work/github.mode"

chmod 644 "$work/config/llmgate/token.json"
doctor --offline
label="a token file other users can read fails with the chmod remedy"
check sh -c "[ $rc -ne 0 ] && grep -q 'chmod 600' '$work/doctor.out' && ! grep -q '$secret' '$work/doctor.out'"
chmod 600 "$work/config/llmgate/token.json"

mv "$work/config/llmgate/token.json" "$work/config/llmgate/token.bak"
doctor --offline
label="a missing token file fails and points at llmgate login"
check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  copilot-login' '$work/doctor.out' && grep -q 'fix: run llmgate login' '$work/doctor.out'"
mv "$work/config/llmgate/token.bak" "$work/config/llmgate/token.json"

"$binary" --config "$config" claude --start >/dev/null
doctor --offline
label="a running gateway of this user passes the port check"
check sh -c "[ $rc -eq 0 ] && grep -q 'PASS  port: this user' '$work/doctor.out'"

chmod 644 "$work/state/llmgate/identity"
doctor --offline
label="an identity file other users can read fails"
check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  identity' '$work/doctor.out' && grep -q 'chmod 600' '$work/doctor.out'"
chmod 600 "$work/state/llmgate/identity"
"$binary" --config "$config" stop >/dev/null

python3 - "$port" <<'PY' &
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'{"status":"ok"}'; self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
squatter_pid=$!
while ! python3 -c "import socket; socket.create_connection(('127.0.0.1',$port),1)" 2>/dev/null; do sleep 0.1; done
doctor --offline
kill $squatter_pid 2>/dev/null || true
label="something else answering on the port fails"
check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  port' '$work/doctor.out'"

sed "s/127.0.0.1:$port/0.0.0.0:$port/" "$config" > "$work/open.jsonc"
if "$binary" --config "$work/open.jsonc" doctor --offline > "$work/doctor.out" 2>&1; then rc=0; else rc=$?; fi
label="an open listen address without a client key fails"
check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  listen' '$work/doctor.out'"

echo '{ "listen": ' > "$work/broken.jsonc"
if "$binary" --config "$work/broken.jsonc" doctor > "$work/doctor.out" 2>&1; then rc=0; else rc=$?; fi
label="a configuration that does not parse is reported by doctor itself"
check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  config' '$work/doctor.out' && grep -q 'FAIL  config' '$work/doctor.out'"

mkdir -p "$work/ro"
chmod 500 "$work/ro"
sed "s#$work/usage.jsonl#$work/ro/sub/usage.jsonl#" "$config" > "$work/ledger.jsonc"
if [ "$(id -u)" -ne 0 ]; then
  if "$binary" --config "$work/ledger.jsonc" doctor --offline > "$work/doctor.out" 2>&1; then rc=0; else rc=$?; fi
  label="a ledger that cannot be created fails"
  check sh -c "[ $rc -ne 0 ] && grep -q 'FAIL  ledger' '$work/doctor.out'"
fi
chmod 700 "$work/ro"

echo "failures: $failures"
[ "$failures" -eq 0 ]
