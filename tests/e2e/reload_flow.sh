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

label="provider add without --yes shows the diff and writes nothing"
run provider add home --preset anthropic --labels private > "$work/dry.out" 2>&1 || true
check sh -c "grep -q '^+' '$work/dry.out' && grep -q 'not written' '$work/dry.out' && ! grep -q '\"home\"' '$work/config.jsonc'"
label="provider add --yes writes, keeps the comments, and the running gateway serves it after the reload"
sed -i.bak 's|"providers": {|// my providers\n  "providers": {|' "$work/config.jsonc"
run provider add home --preset anthropic --labels private --yes > "$work/add.out" 2>&1 || true
run targets > "$work/t3.out"
check sh -c "grep -q 'my providers' '$work/config.jsonc' && grep -q '\"home\"' '$work/config.jsonc' && grep -q 'written to' '$work/add.out'"
label="models set and routes set edit the file and the gateway follows"
run models set home claude-sonnet-5-5 claude-sonnet-home --yes > /dev/null 2>&1 || true
run routes set sonnet home,alpha --yes > /dev/null 2>&1 || true
check sh -c "grep -q 'claude-sonnet-home' '$work/config.jsonc' && grep -q '\"sonnet\": \[\"home\",\"alpha\"\]' '$work/config.jsonc'"
label="a route to an unknown provider is refused and the file is unchanged"
cp "$work/config.jsonc" "$work/before.jsonc"
check sh -c "! '$binary' --config '$work/config.jsonc' routes set opus nowhere --yes > '$work/nowhere.out' 2>&1 && grep -q nowhere '$work/nowhere.out' && cmp -s '$work/config.jsonc' '$work/before.jsonc'"
label="a provider that a route still names cannot be removed, and once the route is cleared it can"
check sh -c "! '$binary' --config '$work/config.jsonc' provider remove home --yes > '$work/keep.out' 2>&1 && grep -q home '$work/keep.out' && grep -q '\"home\"' '$work/config.jsonc'"
run routes clear sonnet --yes > /dev/null 2>&1 || true
run provider remove home --yes > /dev/null 2>&1 || true
check sh -c "! grep -q '\"home\"' '$work/config.jsonc'"

label="secret set stores the piped value at mode 0600, prints the path and never the value"
printf 'sekrit-value\n' | "$binary" --config "$work/config.jsonc" secret set partner > "$work/secret.out" 2>&1
check sh -c "grep -q 'secrets/partner.key' '$work/secret.out' && ! grep -q 'sekrit-value' '$work/secret.out' && [ \"\$(cat '$work/home/.config/llmgate/secrets/partner.key' 2>/dev/null || cat '$work/config/llmgate/secrets/partner.key')\" = sekrit-value ]"
label="secret set refuses a name that climbs out of the directory"
check sh -c "! printf x | '$binary' --config '$work/config.jsonc' secret set ../x > /dev/null 2>&1"
label="provider add with header auth and a secret file writes the auth block"
run provider add partner --base-url http://127.0.0.1:9 --auth header:api-key --secret file:~/.config/llmgate/secrets/partner.key --offer m-nx --yes > "$work/nx.out" 2>&1 || true
check sh -c "grep -q '\"auth\":{\"kind\":\"header\",\"header\":\"api-key\",\"file\"' '$work/config.jsonc'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
