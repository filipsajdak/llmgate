#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
binary=${1:?usage: shutdown_flow.sh <llmgate binary>}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
export E2E_UPSTREAM_TOKEN=test-token
failures=0
started=$(date +%s)
check() { elapsed=$(($(date +%s) - started)); if "$@"; then echo "ok   [${elapsed}s] $label"; else echo "FAIL [${elapsed}s] $label"; failures=$((failures + 1)); fi; }
. "$(dirname "$0")/diagnose.sh"
watchdog_seconds=100
script_pid=$$
( sleep "$watchdog_seconds"; kill -0 "$script_pid" 2>/dev/null && kill -TERM "$script_pid" ) < /dev/null > /dev/null 2>&1 &
watchdog_pid=$!
cleanup() {
  status=$?
  kill "$watchdog_pid" 2>/dev/null || true
  if [ "${timed_out:-0}" = 1 ]; then
    echo "== watchdog: still running after $watchdog_seconds s"
  fi
  if [ "$status" -ne 0 ]; then
    echo "== the script ended with status $status"
    diagnose_gateway
  fi
  kill $(cat "$work/state/llmgate/gateway.pid" 2>/dev/null) 2>/dev/null || true
  kill ${gateway_pid:-} ${upstream_pid:-} ${curl_pid:-} 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT
trap 'timed_out=1; exit 143' TERM
trap 'exit 130' INT

cat > "$work/upstream.py" <<'PY'
import fast_http, http.server, sys, time
delay = float(sys.argv[2])
BODY = b'{"id":"msg_e2e","type":"message","role":"assistant","model":"m","content":[],"stop_reason":"end_turn","usage":{"input_tokens":3,"output_tokens":4}}'
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get("content-length", 0)))
        time.sleep(delay)
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
server.daemon_threads = True
open(sys.argv[1], "w").write(str(server.server_address[1]))
server.serve_forever()
PY

free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

start_upstream() {
  rm -f "$work/upstream.port"
  python3 "$work/upstream.py" "$work/upstream.port" "$1" &
  upstream_pid=$!
  while [ ! -s "$work/upstream.port" ]; do sleep 0.1; done
  upstream_port=$(cat "$work/upstream.port")
}

write_config() {
  port=$(free_port)
  cat > "$work/llmgate.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "upstream": { "base_url": "http://127.0.0.1:$upstream_port" },
  "static_token_env": "E2E_UPSTREAM_TOKEN",
  "time": { "drain": "$1" },
  "claude": { "profile_dir": "$work/profile" },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
  config="$work/llmgate.jsonc"
}

start_gateway() {
  : > "$work/gateway.log"
  "$binary" --config "$config" serve > "$work/gateway.log" 2>&1 &
  gateway_pid=$!
  while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done
}

slow_request() {
  curl -s -o "$work/body.out" -w '%{http_code}' --max-time 20 -X POST "http://127.0.0.1:$port/anthropic/v1/messages" \
    -H 'content-type: application/json' -d '{"model":"claude-sonnet-5-5","max_tokens":1,"messages":[]}' > "$work/code.out" 2>/dev/null &
  curl_pid=$!
}

gateway_exit() { if wait "$gateway_pid"; then exit_status=0; else exit_status=$?; fi; }

stop_upstream() { kill "$upstream_pid" 2>/dev/null || true; wait "$upstream_pid" 2>/dev/null || true; }

start_upstream 1.5
write_config 10s
start_gateway
slow_request
sleep 0.5
kill -TERM "$gateway_pid"
gateway_exit
wait "$curl_pid" || true
label="SIGTERM lets the request in flight finish and the gateway exits 0"
check sh -c "[ $exit_status -eq 0 ] && [ \"\$(cat '$work/code.out')\" = 200 ] && grep -q 'llmgate stopped' '$work/gateway.log'"
label="the finished request reached the usage ledger"
check sh -c "grep -q '\"status\":200' '$work/usage.jsonl'"

start_gateway
sleep 0.3
kill -INT "$gateway_pid"
gateway_exit
label="SIGINT stops an idle gateway with status 0"
check test "$exit_status" -eq 0
stop_upstream

start_upstream 6
write_config 10s
start_gateway
slow_request
sleep 0.5
began=$(date +%s)
kill -TERM "$gateway_pid"
sleep 0.3
kill -TERM "$gateway_pid"
gateway_exit
took=$(( $(date +%s) - began ))
kill "$curl_pid" 2>/dev/null || true
label="a second signal exits at once with status 130"
check sh -c "[ $exit_status -eq 130 ] && [ $took -le 3 ]"
stop_upstream

start_upstream 6
write_config 300ms
start_gateway
slow_request
sleep 0.5
began=$(date +%s)
kill -TERM "$gateway_pid"
gateway_exit
took=$(( $(date +%s) - began ))
kill "$curl_pid" 2>/dev/null || true
label="the drain timeout abandons a request that runs too long and still exits 0"
check sh -c "[ $exit_status -eq 0 ] && [ $took -le 3 ] && grep -q 'still running' '$work/gateway.log'"
stop_upstream

start_upstream 1.5
write_config 10s
printf '#!/bin/sh\nexit 0\n' > "$work/claude"
chmod +x "$work/claude"
LLMGATE_CLAUDE="$work/claude" "$binary" --config "$config" claude >/dev/null
recorded=$(cat "$work/state/llmgate/gateway.pid")
slow_request
sleep 0.4
stop_output=$("$binary" --config "$config" stop)
wait "$curl_pid" || true
label="stop waits for the gateway: the request in flight finished and the process is gone"
check sh -c "echo '$stop_output' | grep -q 'gateway stopped' && [ \"\$(cat '$work/code.out')\" = 200 ] && ! kill -0 $recorded 2>/dev/null"
label="the gateway left no pid file behind"
check test ! -e "$work/state/llmgate/gateway.pid"
stop_upstream

echo "failures: $failures"
[ "$failures" -eq 0 ]
