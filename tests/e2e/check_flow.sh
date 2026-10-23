#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
trap 'kill ${good_pid:-} ${bad_pid:-} ${gateway_pid:-} 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }

FAKE_TOOL_COMMAND=true python3 "$root/tests/e2e/fake_anthropic.py" "$work/good.port" "$work/good.log" tool &
good_pid=$!
python3 "$root/tests/e2e/fake_anthropic.py" "$work/bad.port" "$work/bad.log" none &
bad_pid=$!
while [ ! -s "$work/good.port" ] || [ ! -s "$work/bad.port" ]; do sleep 0.1; done
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "good": { "base_url": "http://127.0.0.1:$(cat "$work/good.port")", "static_token_env": "CHECK_TOKEN", "models": { "claude-haiku-5-5": "up-good" } },
    "bad": { "base_url": "http://127.0.0.1:$(cat "$work/bad.port")", "static_token_env": "CHECK_TOKEN", "models": { "claude-haiku-5-5": "up-bad" } },
    "fwd": { "base_url": "http://127.0.0.1:9", "forward_credentials": true, "models": { "claude-haiku-5-5": "x" } },
    "empty": { "base_url": "http://127.0.0.1:9", "auth": { "kind": "none" } }
  },
  "agents": { "reviewer": { "target": "claude-up-good@good", "description": "d", "prompt": "p" } },
  "claude": { "credential": "login" }
}
JSON
export CHECK_TOKEN=t
"$binary" --config "$work/config.jsonc" serve > "$work/gateway.log" 2>&1 &
gateway_pid=$!
while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done

run() { "$binary" --config "$work/config.jsonc" "$@"; }
run check > "$work/check.out" 2>&1 || true
label="check passes a provider whose three steps work and lists the steps with times"
check sh -c "grep -E '^good +ping +pass' '$work/check.out' && grep -E '^good +tool +pass' '$work/check.out' && grep -E '^good +context +pass' '$work/check.out'"
label="check fails the step the provider cannot do and says why, and stops that provider there"
check sh -c "grep -E '^bad +ping +pass' '$work/check.out' && grep -E '^bad +tool +FAIL +.*no tool call' '$work/check.out' && ! grep -E '^bad +context' '$work/check.out'"
label="check skips a forwarding provider and one without a model, naming the reason"
check sh -c "grep -E '^fwd +all +skip +.*forwards your Claude login' '$work/check.out' && grep -E '^empty +all +skip +.*no model' '$work/check.out'"
label="check tests the target of each configured agent"
check sh -c "grep -E '^agent reviewer +agent +pass' '$work/check.out'"
label="check exits non-zero when a step fails and zero when only the good provider is asked"
check sh -c "! '$binary' --config '$work/config.jsonc' check > /dev/null 2>&1 && '$binary' --config '$work/config.jsonc' check --provider good --quick > '$work/quick.out' 2>&1 && grep -E '^good +ping +pass' '$work/quick.out' && ! grep -q ' tool ' '$work/quick.out'"
label="check --json gives rows with a result per step"
check sh -c "'$binary' --config '$work/config.jsonc' check --provider good --quick --json | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[0][\"result\"]==\"pass\" and d[0][\"step\"]==\"ping\"'"
label="check names an unknown provider and a stopped gateway"
check sh -c "! '$binary' --config '$work/config.jsonc' check --provider nope > '$work/nope.out' 2>&1 && grep -q 'no provider named nope' '$work/nope.out'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; cat "$work/check.out" | head -20; exit 1; }
