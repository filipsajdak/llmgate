#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
trap 'kill $upstream_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

python3 "$root/tests/e2e/fake_anthropic.py" "$work/upstream.port" "$work/upstream.log" tool &
upstream_pid=$!
while [ ! -s "$work/upstream.port" ]; do sleep 0.1; done
upstream_port=$(cat "$work/upstream.port")

cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:0",
  "upstream": { "base_url": "http://127.0.0.1:$upstream_port", "messages_path": "/v1/messages" },
  "models": { "sonnet": "fake-sonnet-upstream" },
  "static_token_env": "LLMGATE_TEST_TOKEN",
  "client_key_env": "LLMGATE_CLIENT_KEY",
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
LLMGATE_TEST_TOKEN=fake-upstream-token LLMGATE_CLIENT_KEY=local-client-key "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
gateway_pid=$!
while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done
gateway_port=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$work/gateway.log")

python3 - "$gateway_port" "$work/usage.jsonl" <<'PY'
import http.client
import json
import sys

port, ledger = int(sys.argv[1]), sys.argv[2]
KEY = "local-client-key"
TOOLS = [{"name": "Bash", "description": "run a command", "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}}}]
failures = []


def check(label, condition):
    print(("ok   " if condition else "FAIL ") + label)
    if not condition:
        failures.append(label)


def post(messages):
    body = json.dumps({"model": "sonnet", "max_tokens": 64, "stream": True, "tools": TOOLS, "messages": messages})
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("POST", "/anthropic/v1/messages", body, {"x-api-key": KEY, "content-type": "application/json"})
    reply = connection.getresponse()
    text = reply.read().decode()
    events = [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ")]
    return reply.status, events


def get(path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("GET", path, headers={"x-api-key": KEY})
    return json.loads(connection.getresponse().read())


user = {"role": "user", "content": "run echo tool-ran"}
status, first = post([user])
kinds = [event["type"] for event in first]
check("turn 1 answers 200", status == 200)
check("turn 1 streams a tool_use block", any(e.get("content_block", {}).get("type") == "tool_use" for e in first))
check("turn 1 keeps the event order",
      kinds == ["message_start", "content_block_start", "content_block_delta", "content_block_delta", "content_block_stop", "message_delta", "message_stop"])
check("turn 1 input fragments rebuild the command",
      "".join(e["delta"]["partial_json"] for e in first if e["type"] == "content_block_delta") == '{"command":"echo tool-ran"}')
check("turn 1 stops for the tool", next(e for e in first if e["type"] == "message_delta")["delta"]["stop_reason"] == "tool_use")

assistant = {"role": "assistant", "content": [{"type": "tool_use", "id": "toolu_fake1", "name": "Bash", "input": {"command": "echo tool-ran"}}]}
result = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_fake1", "content": "tool-ran"}]}
status, second = post([user, assistant, result])
check("turn 2 answers 200", status == 200)
check("turn 2 carries the final text", any(e.get("delta", {}).get("text") == "Tool finished: all good" for e in second))
check("turn 2 ends the turn", next(e for e in second if e["type"] == "message_delta")["delta"]["stop_reason"] == "end_turn")

stats = get("/llmgate/stats")
check("both turns were streamed", stats["streamed"] == 2)
check("no upstream failure", stats["upstream_failures"] == 0)
check("no stream repair was needed", all(count == 0 for count in stats["repairs"].values()))
check("the second turn reused the upstream connection", stats["connections_reused"] == 1 and stats["connections_opened"] == 1)
lines = [json.loads(line) for line in open(ledger)]
check("the ledger holds one line per turn", len(lines) == 2)
check("every ledger line names the provider, counts tokens and records status 200",
      all(line["provider"] == "default" and line["input"] + line["output"] > 0 and line["status"] == 200 and line["repairs"] == 0 for line in lines))
check("the ledger records the model the client asked for", all(line["requested"] == "sonnet" for line in lines))
sys.exit(1 if failures else 0)
PY
