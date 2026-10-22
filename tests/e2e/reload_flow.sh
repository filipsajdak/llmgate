#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
trap 'kill ${gateway_pid:-} 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

port=$(free_port)
write_config() {
    cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "alpha": { "base_url": "http://127.0.0.1:9", "static_token_env": "E2E_TOKEN", "offer": ["$1"], "labels": ["work"] }$2
  }
}
JSON
}
export E2E_TOKEN=e2e-token
run() { "$binary" --config "$work/config.jsonc" "$@"; }

write_config m-one ""
run serve > "$work/serve.out" 2>&1 &
gateway_pid=$!
tries=0
until grep -q listening "$work/serve.out" 2>/dev/null; do
    tries=$((tries + 1))
    [ "$tries" -lt 100 ] || { echo "the gateway did not start"; cat "$work/serve.out"; exit 1; }
    sleep 0.1
done

label="the running gateway lists the model of the first config"
check sh -c "'$binary' --config '$work/config.jsonc' targets | grep -q 'claude-m-one@alpha'"

write_config m-two ',
    "beta": { "base_url": "http://127.0.0.1:9", "static_token_env": "E2E_TOKEN", "offer": ["m-three"] }'
label="reload applies the edited file to the running gateway"
check sh -c "'$binary' --config '$work/config.jsonc' reload > '$work/reload.out' 2>&1 && grep -q '^ok' '$work/reload.out'"
run targets > "$work/targets.out"
label="the new provider and model are served and the old model is gone"
check sh -c "grep -q 'claude-m-two@alpha' '$work/targets.out' && grep -q 'claude-m-three@beta' '$work/targets.out' && ! grep -q 'claude-m-one@alpha' '$work/targets.out'"
label="the label of the provider reaches the display name"
check sh -c "grep -q 'm-two (alpha) \\[work\\]' '$work/targets.out'"

echo '{ "listen": ' > "$work/config.jsonc"
label="a config that does not parse is refused, says why and changes nothing"
check sh -c "! '$binary' --config '$work/config.jsonc' reload > '$work/bad.out' 2>&1; true"
write_config m-two ',
    "beta": { "base_url": "http://127.0.0.1:9", "static_token_env": "E2E_TOKEN", "offer": ["m-three"] }'
run targets > "$work/after.out"
check sh -c "grep -q 'claude-m-two@alpha' '$work/after.out'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
