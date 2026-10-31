#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/.." && cd .. && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
trap 'kill $tool_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

FAKE_REQUIRE_AUTHORIZATION="Bearer fake-bedrock-key" FAKE_TOOL_COMMAND="echo converse-ran" python3 "$root/tests/e2e/fake_bedrock.py" "$work/tool.port" "$work/tool.log" tool &
tool_pid=$!
while [ ! -s "$work/tool.port" ]; do sleep 0.1; done

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "nova": { "base_url": "http://127.0.0.1:$(cat "$work/tool.port")", "protocol": "bedrock_messages", "auth": { "kind": "bearer", "env": "BEDROCK_KEY" },
              "model_protocols": { "amazon.nova-pro-v1:0": "bedrock_converse" },
              "models": { "claude-sonnet-5-5": "amazon.nova-pro-v1:0" } },
    "thinker": { "base_url": "http://127.0.0.1:$(cat "$work/tool.port")", "protocol": "bedrock_converse", "auth": { "kind": "bearer", "env": "BEDROCK_KEY" },
                 "bedrock": { "converse": { "thinking_models": ["amazon.nova-pro-v1:0"], "drop_cache_points": true } },
                 "models": { "claude-haiku-5-5": "amazon.nova-pro-v1:0" } }
  },
  "routes": { "other": ["nova"], "haiku": ["thinker"] },
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
LONG_NAME = "mcp__" + "a" * 75
TOOLS = [{"name": "Bash", "description": "run a command", "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}, "cache_control": {"type": "ephemeral"}},
         {"name": LONG_NAME, "description": "long", "input_schema": {"type": "object"}}]
PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="
failures = []


def check(label, condition):
    print(("ok   " if condition else "FAIL ") + label)
    if not condition:
        failures.append(label)


def post(payload):
    fields = dict({"model": "claude-sonnet-5-5", "max_tokens": 64, "stream": True, "tools": TOOLS, "thinking": {"type": "enabled", "budget_tokens": 2000}, "metadata": {"user_id": "u"},
                   "system": [{"type": "text", "text": "Be brief.", "cache_control": {"type": "ephemeral"}}]}, **payload)
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("POST", "/anthropic/v1/messages", json.dumps({k: v for k, v in fields.items() if v is not None}).encode(),
                       {"content-type": "application/json", "anthropic-version": "2023-06-01", "x-api-key": "client-secret-xyz"})
    reply = connection.getresponse()
    text = reply.read().decode()
    return reply.status, [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ")], text


def get(path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("GET", path)
    return json.loads(connection.getresponse().read())


user = {"role": "user", "content": [{"type": "text", "text": "run echo converse-ran", "cache_control": {"type": "ephemeral"}},
                                    {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG}},
                                    {"type": "document", "source": {"type": "base64", "media_type": "application/pdf", "data": "JVBERi0="}}]}
status, first, _ = post({"messages": [user]})
kinds = [event["type"] for event in first]
check("turn 1 streams a tool call with the original block shape", status == 200 and kinds[0] == "message_start" and any(e.get("content_block", {}).get("name") == "Bash" for e in first))
check("the tool input fragments rebuild the command", "".join(e["delta"]["partial_json"] for e in first if e["type"] == "content_block_delta") == '{"command": "echo converse-ran"}')
check("turn 1 ends for the tool, with message_delta then message_stop", next(e for e in first if e["type"] == "message_delta")["delta"]["stop_reason"] == "tool_use" and kinds[-2:] == ["message_delta", "message_stop"])

assistant = {"role": "assistant", "content": [{"type": "tool_use", "id": "tooluse_fake", "name": "Bash", "input": {"command": "echo converse-ran"}}]}
result = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "tooluse_fake", "content": "converse-ran"}]}
status, second, _ = post({"messages": [user, assistant, result]})
usage = next(e for e in second if e["type"] == "message_delta")["usage"]
check("turn 2 carries the final text", "".join(e["delta"].get("text", "") for e in second if e["type"] == "content_block_delta") == "Tool finished: all good")
check("usage arrives before message_stop with the cache counts split", [e["type"] for e in second][-2:] == ["message_delta", "message_stop"] and usage["input_tokens"] == 20 and usage["cache_read_input_tokens"] == 100 and usage["cache_creation_input_tokens"] == 3)

logged = [json.loads(line) for line in open(f"{work}/tool.log")]
check("the upstream saw the converse-stream path with the model id percent-encoded", all(r["path"] == "/model/amazon.nova-pro-v1%3A0/converse-stream" for r in logged))
check("the upstream saw the bearer key and no Anthropic header", all(r["headers"].get("authorization") == "Bearer fake-bedrock-key" and "x-api-key" not in r["headers"] and "anthropic-version" not in r["headers"] for r in logged))
sent = logged[0]["request"]
check("the body is a Converse body without model, stream, thinking or metadata", all(k not in sent for k in ("model", "stream", "thinking", "metadata")) and sent["inferenceConfig"] == {"maxTokens": 64})
blocks = sent["messages"][0]["content"]
check("text, image and pdf become Converse blocks, each cache point after its block", blocks[0]["text"].startswith("run echo") and "cachePoint" in blocks[1] and blocks[2]["image"]["format"] == "png" and blocks[3]["document"]["format"] == "pdf")
check("the system prompt carries its cache point", sent["system"][0]["text"] == "Be brief." and "cachePoint" in sent["system"][1])
specs = [item["toolSpec"] for item in sent["toolConfig"]["tools"] if "toolSpec" in item]
check("tools become toolSpecs and a name over 64 characters is hashed", specs[0]["name"] == "Bash" and len(specs[1]["name"]) == 64 and specs[1]["name"] != LONG_NAME and "cachePoint" in sent["toolConfig"]["tools"][1])
check("the tool result keeps its id", logged[1]["request"]["messages"][2]["content"][0]["toolResult"]["toolUseId"] == "tooluse_fake")

status, events, _ = post({"messages": [{"role": "user", "content": "FAKE_EXCEPTION"}]})
check("an exception frame becomes an Anthropic error event of the matching type", events[-1]["type"] == "error" and events[-1]["error"]["type"] == "rate_limit_error")
status, _, text = post({"messages": [user], "stream": False})
check("a request that is not streamed is refused with a clear message", status == 400 and "stream must be true" in text)

for choice, expected in (({"type": "auto"}, {"auto": {}}), ({"type": "any"}, {"any": {}}), ({"type": "tool", "name": "Bash"}, {"tool": {"name": "Bash"}})):
    post({"messages": [{"role": "user", "content": "choose"}], "tool_choice": choice})
    check(f"tool_choice {choice['type']} maps to Converse toolChoice", json.loads(open(f"{work}/tool.log").readlines()[-1])["request"]["toolConfig"]["toolChoice"] == expected)
post({"messages": [{"role": "user", "content": "choose"}], "tool_choice": {"type": "none"}})
check("tool_choice none sends no tools at all", "toolConfig" not in json.loads(open(f"{work}/tool.log").readlines()[-1])["request"])

post({"model": "claude-haiku-5-5", "messages": [user]})
thinker = json.loads(open(f"{work}/tool.log").readlines()[-1])["request"]
check("a model listed in thinking_models gets thinking in additionalModelRequestFields", thinker["additionalModelRequestFields"]["thinking"]["type"] == "enabled" and "thinking" not in thinker)
check("drop_cache_points removes the cache points", all("cachePoint" not in block for block in thinker["messages"][0]["content"]) and len(thinker["system"]) == 1)

stats = get("/llmgate/stats")
check("the dropped fields are counted by name for a model without thinking", stats["dropped_fields"].get("thinking", 0) >= 3 and stats["dropped_fields"].get("metadata", 0) >= 3)
lines = [json.loads(line) for line in open(f"{work}/usage.jsonl")]
check("the ledger holds the turns with input and cache read split", any(line["provider"] == "nova" and line["cache_read"] == 100 and line["input"] == 20 and line["output"] == 9 for line in lines))
sys.exit(1 if failures else 0)
PY
