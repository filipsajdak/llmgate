#!/bin/sh
set -eu
binary=${1:?usage: detached_pipe_flow.sh <llmgate binary> <claude_stub>}
claude_stub=${2:?usage: detached_pipe_flow.sh <llmgate binary> <claude_stub>}
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
config="$work/llmgate.jsonc"
trap '"$binary" --config "$config" stop >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$config" <<JSON
{
  "listen": "127.0.0.1:$port",
  "upstream": { "base_url": "http://127.0.0.1:1" },
  "static_token_env": "E2E_UPSTREAM_TOKEN",
  "claude": { "mod": false, "profile_dir": "$work/profile" }
}
JSON
case "$claude_stub" in *.exe) stub_suffix=.exe ;; *) stub_suffix= ;; esac
cp "$claude_stub" "$work/claude$stub_suffix"
export E2E_UPSTREAM_TOKEN=test-token STUB_OUTPUT="$work/claude.out" LLMGATE_CLAUDE="$work/claude"

captured=$("$binary" --config "$config" claude 2>&1)
echo "ok   a pipe that reads llmgate claude reached end of file while the gateway was still running"

"$binary" --config "$config" status | grep -q "^gateway: up"
echo "ok   the detached gateway outlived the command whose output was captured"
