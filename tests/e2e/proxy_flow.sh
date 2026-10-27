#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
pki=$root/tests/e2e/pki
work=$(mktemp -d)
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
failures=0
check() { if "$@"; then echo "ok   $label"; else echo "FAIL $label"; failures=$((failures + 1)); fi; }
trap 'kill $server_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT
unset HTTPS_PROXY https_proxy ALL_PROXY all_proxy NO_PROXY no_proxy SSL_CERT_FILE SSL_CERT_DIR

python3 "$root/tests/e2e/fake_proxy.py" "$pki" "$work/ports.json" "$work/proxy.log" &
server_pid=$!
while [ ! -s "$work/ports.json" ]; do sleep 0.1; done
port() { python3 -c "import json; print(json.load(open('$work/ports.json'))['$1'])"; }
origin=$(port origin)

write_config() {
  cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:1",
  "providers": {
    "p": { "base_url": "https://origin.test", "auth": { "kind": "none" } },
    "q": { "base_url": "https://localhost:$origin", "auth": { "kind": "none" } },
    "r": { "base_url": "https://192.0.2.7", "auth": { "kind": "none" } }
  },
  "network": { "system_roots": false$1 }
}
JSON
}
models() { env "$@"; }
connects() { grep -c "CONNECT" "$work/proxy.log" || true; }

write_config ""
label="a plain proxy named in the environment carries the request and the provider answers"
models HTTPS_PROXY=http://127.0.0.1:$(port plain) SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1 || true
check sh -c "grep -q proxied-model '$work/out.txt' && grep -q 'plain CONNECT origin.test:443 auth=absent' '$work/proxy.log'"

write_config ", \"proxy\": \"http://127.0.0.1:$(port plain)\""
label="network.proxy wins over the environment"
before=$(connects)
models HTTPS_PROXY=http://127.0.0.1:1 SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1 || true
check sh -c "grep -q proxied-model '$work/out.txt' && [ \"\$(grep -c CONNECT '$work/proxy.log')\" -gt $before ]"

write_config ", \"proxy\": \"http://127.0.0.1:$(port plain)\", \"no_proxy\": [\"Origin.Test\"]"
label="network.no_proxy keeps a host off the proxy, whatever its case"
before=$(connects)
if models SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && [ \"\$(grep -c CONNECT '$work/proxy.log')\" -eq $before ]"

write_config ", \"proxy\": \"http://127.0.0.1:$(port plain)\", \"no_proxy\": [\"10.0.0.0/8\", \"fd00::/8\", \".corp.test\", \"svc.test:8443\"]"
label="a host given as an address that no bypass entry covers goes through the proxy"
models SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider r > "$work/out.txt" 2>&1 || true
check sh -c "grep -q proxied-model '$work/out.txt' && grep -q 'plain CONNECT 192.0.2.7:443' '$work/proxy.log'"

write_config ""
label="NO_PROXY keeps a host off the proxy: no tunnel is opened and the failure is the host's own"
before=$(connects)
if models HTTPS_PROXY=http://127.0.0.1:$(port plain) NO_PROXY=origin.test SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && [ \"\$(grep -c CONNECT '$work/proxy.log')\" -eq $before ]"

label="a loopback provider is never proxied, even when the proxy is dead"
models HTTPS_PROXY=http://127.0.0.1:1 SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider q > "$work/out.txt" 2>&1 || true
check grep -q proxied-model "$work/out.txt"

label="a proxy that asks for credentials is named, and nothing is retried"
if models HTTPS_PROXY=http://127.0.0.1:$(port login) SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'proxy asks for credentials' '$work/out.txt'"

label="credentials in the proxy URL are sent as Basic and never printed"
models HTTPS_PROXY=http://user:pass@127.0.0.1:$(port login) SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1 || true
check sh -c "grep -q proxied-model '$work/out.txt' && grep -q 'login CONNECT origin.test:443 auth=present' '$work/proxy.log' && ! grep -q 'pass' '$work/out.txt' && ! grep -q 'pass' '$work/proxy.log'"

label="a proxy that refuses the tunnel is named as the cause"
if models HTTPS_PROXY=http://127.0.0.1:$(port deny) SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'proxy refused to open a tunnel' '$work/out.txt'"

label="an intercepting proxy is refused with the reason and the remedy while its authority is not trusted"
if models HTTPS_PROXY=http://127.0.0.1:$(port intercept) SSL_CERT_FILE=$pki/testca.pem "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'authority this machine does not trust' '$work/out.txt' && grep -q 'network.ca_file' '$work/out.txt'"

write_config ", \"ca_file\": \"$pki/corpca.pem\""
label="the same proxy works once network.ca_file trusts its authority"
models HTTPS_PROXY=http://127.0.0.1:$(port intercept) "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1 || true
check sh -c "grep -q proxied-model '$work/out.txt' && grep -q 'intercept saw GET' '$work/proxy.log'"

write_config ", \"ca_file\": \"$work/missing.pem\""
label="a network.ca_file that cannot be read is refused by name"
if models "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'network.ca_file cannot be read' '$work/out.txt'"

write_config ""
label="a proxy URL that is not http:// is refused by name"
if models HTTPS_PROXY=ftp://proxy.example "$binary" --config "$work/config.jsonc" models --provider p > "$work/out.txt" 2>&1; then rc=0; else rc=$?; fi
check sh -c "[ $rc -ne 0 ] && grep -q 'must be an http:// URL' '$work/out.txt'"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed"; exit 1; }
echo "all proxy checks passed"
