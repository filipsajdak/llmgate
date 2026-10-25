#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
trap 'kill $upstream_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

python3 "$root/tests/e2e/fake_models.py" "$work/up.port" "$work/up.log" e2e-token &
upstream_pid=$!
while [ ! -s "$work/up.port" ]; do sleep 0.1; done

cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$(free_port)",
  "providers": {
    "p": { "base_url": "http://127.0.0.1:$(cat "$work/up.port")", "static_token_env": "E2E_TOKEN",
           "models": { "claude-on": "up-on", "claude-off": "up-off", "claude-gone": "up-gone" } },
    "q": { "base_url": "http://127.0.0.1:$(cat "$work/up.port")", "static_token_env": "E2E_TOKEN" }
  }
}
JSON
export E2E_TOKEN=e2e-token

run() { "$binary" --config "$work/config.jsonc" "$@"; }
label="models lists each model with its limits and marks the disabled one"
check sh -c "run() { '$binary' --config '$work/config.jsonc' \"\$@\"; }; run models --provider p | grep -q 'up-on  context 300000  prompt 200000  output 100000' && run models --provider p | grep 'up-off' | grep -q 'disabled by the account'"
label="models --json prints the provider's own reply"
check sh -c "'$binary' --config '$work/config.jsonc' models --provider p --json | grep -q '\"policy\"'"
label="models without a provider name names the candidates when there are several"
check sh -c "'$binary' --config '$work/config.jsonc' models 2>&1 | grep -q 'name a provider with --provider (p, q)'"
label="models with a wrong key says the provider answered 401"
check sh -c "E2E_TOKEN=wrong '$binary' --config '$work/config.jsonc' models --provider p 2>&1 | grep -q 'answered 401'"
label="doctor warns for the disabled and the unlisted mapped model, not for the enabled one"
run doctor > "$work/doctor.out" 2>&1 || true
check sh -c "grep -q 'maps claude-off to up-off, which is disabled' '$work/doctor.out' && grep -q 'maps claude-gone to up-gone, which is not listed' '$work/doctor.out' && ! grep -q 'maps claude-on' '$work/doctor.out'"
label="doctor --offline does not list models"
check sh -c "! '$binary' --config '$work/config.jsonc' doctor --offline 2>&1 | grep -q 'maps claude'"

label="models offer previews the offer, the limits and the protocol the model's endpoints need"
run models offer p up-on > "$work/offer.out" 2>&1 || true
check sh -c "grep -q '\"offer\": \[\"up-on\"\]' '$work/offer.out' && grep -q 'context_tokens\":300000' '$work/offer.out' && grep -q 'model_protocols' '$work/offer.out' && grep -q 'openai_responses' '$work/offer.out' && grep -q 'not written' '$work/offer.out'"
label="models offer refuses a disabled model and a chat-only model by name"
check sh -c "! '$binary' --config '$work/config.jsonc' models offer p up-off 2>&1 | grep -qv 'policy' && ! '$binary' --config '$work/config.jsonc' models offer p up-chat 2>&1 | grep -qv '/chat/completions'"
label="models offer all --yes writes only the recommended models"
run models offer p all --yes > "$work/offer-all.out" 2>&1 || true
check sh -c "grep -q 'up-on' '$work/config.jsonc' && ! grep -q 'up-chat\\|\"offer\": \[[^]]*up-off' '$work/config.jsonc'"

label="models list shows every model with its protocol, its window and a recommendation, as text and as JSON"
run models list p > "$work/list.out" 2>&1 || true
run models list p --json > "$work/list.json" 2>&1 || true
check sh -c "grep -q 'up-on  openai_responses  context 300000  enabled  recommended' '$work/list.out' && grep -q 'up-off  anthropic' '$work/list.out' && grep -q 'up-chat  unsupported' '$work/list.out' && python3 -c \"import json,sys; d=json.load(open('$work/list.json')); assert [r['id'] for r in d]==['up-on','up-off','up-chat'] and d[0]['recommended'] and not d[1]['recommended'] and d[2]['protocol']=='unsupported'\""

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
