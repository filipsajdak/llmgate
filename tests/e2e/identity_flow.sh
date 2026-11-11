#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
binary=${1:?usage: identity_flow.sh <llmgate binary> <claude_stub>}
claude_stub=${2:?usage: identity_flow.sh <llmgate binary> <claude_stub>}
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
export HOME="$work/home" XDG_STATE_HOME="$work/state" XDG_CONFIG_HOME="$work/config"
mkdir -p "$HOME"
case "$claude_stub" in *.exe) stub_suffix=.exe ;; *) stub_suffix= ;; esac
cp "$claude_stub" "$work/claude$stub_suffix"
export E2E_UPSTREAM_TOKEN=test-token STUB_OUTPUT="$work/claude.out" LLMGATE_CLAUDE="$work/claude"
trap 'rm -rf "$work"' EXIT
python3 - "$binary" "$work" <<'PY'
import json, os, pathlib, socket, subprocess, sys, time

binary, work = sys.argv[1], pathlib.Path(sys.argv[2])
GATEWAYS = 6
LISTEN_SECONDS = 30


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def listening(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True
    except OSError:
        return False


failures = 0


def check(label, ok):
    global failures
    print(("ok   " if ok else "FAIL ") + label, flush=True)
    failures += 0 if ok else 1


configs = []
for index in range(GATEWAYS):
    port = free_port()
    path = work / f"c{index}.jsonc"
    path.write_text(json.dumps({"listen": f"127.0.0.1:{port}", "upstream": {"base_url": "http://127.0.0.1:1"}, "static_token_env": "E2E_UPSTREAM_TOKEN",
                                "claude": {"mod": False, "profile_dir": str(work / f"profile{index}")}}))
    configs.append((path, port))

processes = [subprocess.Popen([binary, "--config", str(path), "serve"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for path, _ in configs]
try:
    deadline = time.time() + LISTEN_SECONDS
    while time.time() < deadline and not all(listening(port) for _, port in configs):
        time.sleep(0.05)
    check("six gateways that started at once on a fresh state are all listening", all(listening(port) for _, port in configs))
    refused = []
    for path, port in configs:
        done = subprocess.run([binary, "--config", str(path), "claude"], capture_output=True, text=True, stdin=subprocess.DEVNULL)
        if done.returncode != 0:
            refused.append((port, (done.stdout + done.stderr).strip()[-160:]))
    check("the launcher accepts every one of them, so they all hold the key that is in the file" + (f" (refused: {refused})" if refused else ""), not refused)
    identity = (work / "state" / "llmgate" / "identity").read_text()
    check("the identity file holds one complete key", len(identity) == 64)
finally:
    for process in processes:
        process.terminate()
    for process in processes:
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
sys.exit(1 if failures else 0)
PY
