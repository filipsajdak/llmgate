#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
if command -v cygpath >/dev/null 2>&1; then root=$(cygpath -m "$root"); fi
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
trap 'kill $tool_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

path="/v1/projects/flow-project/locations/europe-west1/publishers/anthropic/models/claude-sonnet-5-5:streamRawPredict"
FAKE_REQUIRE_AUTHORIZATION="Bearer fake-vertex-key" FAKE_VERTEX_PATH="$path" python3 "$root/tests/e2e/fake_vertex.py" "$work/tool.port" "$work/tool.log" &
tool_pid=$!
while [ ! -s "$work/tool.port" ]; do sleep 0.1; done

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "vertex": { "base_url": "http://127.0.0.1:$(cat "$work/tool.port")", "preset": "vertex", "region": "europe-west1", "project": "flow-project",
                "auth": { "kind": "bearer", "env": "VERTEX_FLOW_KEY" }, "vertex": { "drop_forced_tool_choice": true },
                "models": { "claude-sonnet-5-5": "claude-sonnet-5-5" } }
  },
  "routes": { "other": ["vertex"] },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
VERTEX_FLOW_KEY=fake-vertex-key "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
gateway_pid=$!
while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done

python3 - "$port" "$work" <<'PY'
import http.client
import json
import sys

port, work = int(sys.argv[1]), sys.argv[2]
TOOLS = [{"name": "Bash", "description": "run a command", "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}}]
failures = []


def check(label, condition):
    print(("ok   " if condition else "FAIL ") + label)
    if not condition:
        failures.append(label)


def post(payload, model="claude-sonnet-5-5"):
    fields = dict({"model": model, "max_tokens": 64, "stream": True, "tools": TOOLS, "system": "Be brief."}, **payload)
    body = json.dumps({key: value for key, value in fields.items() if value is not None})
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("POST", "/anthropic/v1/messages", body.encode(), {"content-type": "application/json", "anthropic-beta": "interleaved-thinking-2025-05-14,unknown-beta-2099-01-01",
                                                                         "anthropic-version": "2023-06-01", "x-claude-code-session-id": "sess-e2e", "x-api-key": "client-secret-xyz"})
    reply = connection.getresponse()
    text = reply.read().decode()
    events = [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ")]
    return reply.status, events, text


def sent():
    return [json.loads(line) for line in open(f"{work}/tool.log")]


status, events, text = post({"messages": [{"role": "user", "content": "run it"}, {"role": "assistant", "content": [{"type": "text", "text": "ok"}]},
                                          {"role": "system", "content": "be brief"}, {"role": "user", "content": "go"}],
                             "tool_choice": {"type": "any"}, "context_management": {"edits": []}})
types = [event.get("type") for event in events]
check("turn 1 answers 200 with the tool call", status == 200 and any(e.get("content_block", {}).get("type") == "tool_use" for e in events))
check("turn 1 starts with message_start and ends with message_stop", types[0] == "message_start" and types[-1] == "message_stop")
first = sent()[0]
check("the upstream saw the project and region path and the bearer key", first["path"].endswith(":streamRawPredict") and "flow-project" in first["path"] and first["authorization"] == "Bearer fake-vertex-key")
check("the body has the vertex version and stream, and neither model nor context_management", first["body"].get("anthropic_version") == "vertex-2023-10-16" and first["body"].get("stream") is True and not {"model", "context_management"} & set(first["body"]))
check("only the known beta reached the body and no anthropic-beta header was sent", first["body"].get("anthropic_beta") == ["interleaved-thinking-2025-05-14"] and first["anthropic_beta_header"] == "")
check("the system message was folded away and the forced tool choice became auto", all(m["role"] != "system" for m in first["body"]["messages"]) and first["body"]["tool_choice"] == {"type": "auto"})
check("the client's own key was not forwarded", "client-secret-xyz" not in json.dumps(first))
usage = [event for event in events if event.get("type") == "message_delta"][0]["usage"]
check("usage carries the cache read tokens", usage.get("cache_read_input_tokens") == 100)

status, events, text = post({"messages": [{"role": "user", "content": "run it"}, {"role": "assistant", "content": [{"type": "tool_use", "id": "toolu_vrtx_fake", "name": "Bash", "input": {"command": "echo tool-ran"}}]},
                                          {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_vrtx_fake", "content": "tool-ran"}]}]})
check("turn 2 carries the final text", status == 200 and "tool done" in text)

status, events, text = post({"stream": False, "messages": [{"role": "user", "content": "hi"}]})
check("a request that is not streamed goes to rawPredict and its JSON answer comes back whole", status == 200 and "whole answer" in text and sent()[-1]["path"].endswith(":rawPredict") and not sent()[-1]["body"].get("stream"))

connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
connection.request("GET", "/llmgate/stats")
stats = json.loads(connection.getresponse().read())
dropped = json.dumps(stats)
check("the dropped field and beta are counted by name", "context_management" in dropped and "anthropic-beta:unknown-beta-2099-01-01" in dropped and "tool_choice" in dropped)

if failures:
    print(f"{len(failures)} check(s) failed")
    sys.exit(1)
PY
