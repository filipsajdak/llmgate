#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
trap 'kill $tool_pid $limited_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

FAKE_REQUIRE_AUTHORIZATION="Bearer fake-bedrock-key" python3 "$root/tests/e2e/fake_bedrock.py" "$work/tool.port" "$work/tool.log" tool &
tool_pid=$!
python3 "$root/tests/e2e/fake_bedrock.py" "$work/limited.port" "$work/limited.log" rate_limited &
limited_pid=$!
while [ ! -s "$work/tool.port" ] || [ ! -s "$work/limited.port" ]; do sleep 0.1; done

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "bedrock": { "base_url": "http://127.0.0.1:$(cat "$work/tool.port")", "protocol": "bedrock_messages", "auth": { "kind": "bearer", "env": "BEDROCK_KEY" },
                 "models": { "claude-sonnet-5-5": "anthropic.claude-sonnet-4-6-v1:0" } },
    "limited": { "base_url": "http://127.0.0.1:$(cat "$work/limited.port")", "protocol": "bedrock_messages", "auth": { "kind": "bearer", "env": "BEDROCK_KEY" },
                 "models": { "claude-opus-5-5": "anthropic.claude-sonnet-4-6-v1:0" } }
  },
  "routes": { "other": ["bedrock"], "opus": ["limited"] },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
BEDROCK_KEY=fake-bedrock-key "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
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
    connection.request("POST", "/anthropic/v1/messages", body.encode(), {"content-type": "application/json", "anthropic-beta": "interleaved-thinking-2025-05-14,unknown-beta-2099-01-01,context-management-2025-06-27",
                                                                         "anthropic-version": "2023-06-01", "x-claude-code-session-id": "sess-e2e", "x-api-key": "client-secret-xyz"})
    reply = connection.getresponse()
    text = reply.read().decode()
    events = [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ")]
    return reply.status, events, text


def get(path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("GET", path)
    return json.loads(connection.getresponse().read())


def last_request():
    return [json.loads(line) for line in open(f"{work}/tool.log")][-1]


user = {"role": "user", "content": "run echo tool-ran"}
status, first, _ = post({"messages": [user]})
kinds = [event["type"] for event in first]
check("turn 1 answers 200 with the tool call", status == 200 and any(e.get("content_block", {}).get("type") == "tool_use" and e["content_block"]["name"] == "Bash" for e in first))
check("turn 1 input fragments rebuild the command", "".join(e["delta"]["partial_json"] for e in first if e["type"] == "content_block_delta") == '{"command": "echo tool-ran"}')
check("turn 1 stops for the tool and ends with message_stop", next(e for e in first if e["type"] == "message_delta")["delta"]["stop_reason"] == "tool_use" and kinds[0] == "message_start" and kinds[-1] == "message_stop")

assistant = {"role": "assistant", "content": [{"type": "tool_use", "id": "toolu_fake", "name": "Bash", "input": {"command": "echo tool-ran"}}]}
result = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_fake", "content": "tool-ran"}]}
status, second, _ = post({"messages": [user, assistant, result]})
check("turn 2 carries the final text", "".join(e["delta"].get("text", "") for e in second if e["type"] == "content_block_delta") == "Tool finished: all good")
usage = next(e for e in second if e["type"] == "message_delta")["usage"]
check("the usage of the Messages events reaches the client unchanged", usage["input_tokens"] == 20 and usage["cache_read_input_tokens"] == 100 and usage["output_tokens"] == 9)

logged = [json.loads(line) for line in open(f"{work}/tool.log")]
check("the upstream saw the invoke path with the model id percent-encoded", all(r["path"] == "/model/anthropic.claude-sonnet-4-6-v1%3A0/invoke-with-response-stream" for r in logged))
check("the upstream saw the bearer key and no client credential or Anthropic header", all(r["headers"].get("authorization") == "Bearer fake-bedrock-key" and "x-api-key" not in r["headers"] and "anthropic-beta" not in r["headers"] and "anthropic-version" not in r["headers"] for r in logged))
sent = logged[0]["request"]
check("the body has no model or stream and carries the Bedrock version", "model" not in sent and "stream" not in sent and sent["anthropic_version"] == "bedrock-2023-05-31")
check("only the allowed beta tokens moved into the body", sent["anthropic_beta"] == ["interleaved-thinking-2025-05-14", "context-management-2025-06-27"])
check("the rest of the body is the client's own", sent["max_tokens"] == 64 and sent["system"] == "Be brief." and sent["tools"][0]["name"] == "Bash")

status, events, _ = post({"messages": [user, {"role": "system", "content": "extra"}]})
roles = [m["role"] for m in last_request()["request"]["messages"]]
check("a mid-conversation system message is folded into the user turn", status == 200 and roles == ["user"] and "extra" in json.dumps(last_request()["request"]["messages"]))

status, _, text = post({"messages": [user], "stream": False})
check("a request that is not streamed is refused with a clear message", status == 400 and "stream must be true" in text)

status, events, _ = post({"messages": [{"role": "user", "content": "FAKE_EXCEPTION"}]})
check("an exception frame becomes an Anthropic error event of the matching type", events[-1]["type"] == "error" and events[-1]["error"]["type"] == "rate_limit_error" and events[-1]["error"]["message"] == "Too many tokens, slow down")
status, events, _ = post({"messages": [{"role": "user", "content": "FAKE_CORRUPT"}]})
check("a corrupt frame ends the stream with an error that names the cause", events[-1]["type"] == "error" and "corrupt" in events[-1]["error"]["message"] and "message_crc" in events[-1]["error"]["message"])
status, _, text = post({"messages": [{"role": "user", "content": "FAKE_400"}]})
check("an upstream 400 reaches Claude Code with Bedrock's own message", status == 400 and json.loads(text)["error"]["type"] == "invalid_request_error" and "Extra inputs are not permitted" in json.loads(text)["error"]["message"])

stats = get("/llmgate/stats")
check("the dropped beta token is counted by name", stats["dropped_fields"].get("anthropic-beta:unknown-beta-2099-01-01", 0) >= 5)
lines = [json.loads(line) for line in open(f"{work}/usage.jsonl")]
first_two = [line for line in lines if line["status"] == 200][:2]
check("the ledger holds the first two answered turns with input and cache read split", len(first_two) == 2 and all(line["provider"] == "bedrock" and line["cache_read"] == 100 and line["input"] == 20 and line["output"] == 9 for line in first_two))

limited_status, _, limited = post({"messages": [user]}, model="claude-opus-5-5")
body = json.loads(limited)
check("an upstream rate limit reaches Claude Code as an Anthropic rate_limit_error with Bedrock's message",
      limited_status == 429 and body["error"]["type"] == "rate_limit_error" and body["error"]["message"].startswith("Too many requests"))
sys.exit(1 if failures else 0)
PY
