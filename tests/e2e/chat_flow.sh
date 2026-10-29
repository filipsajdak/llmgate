#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
trap 'kill $tool_pid $limited_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

FAKE_REQUIRE_KEY=fake-azure-key python3 "$root/tests/e2e/fake_chat.py" "$work/tool.port" "$work/tool.log" tool &
tool_pid=$!
python3 "$root/tests/e2e/fake_chat.py" "$work/limited.port" "$work/limited.log" rate_limited &
limited_pid=$!
while [ ! -s "$work/tool.port" ] || [ ! -s "$work/limited.port" ]; do sleep 0.1; done

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "azure": { "base_url": "http://127.0.0.1:$(cat "$work/tool.port")", "protocol": "openai_chat",
               "messages_path": "/openai/deployments/{model}/chat/completions?api-version=2024-10-21",
               "auth": { "kind": "header", "header": "api-key", "env": "CHAT_KEY" },
               "chat": { "use_max_completion_tokens": true, "drop_stop_sequences": true, "drop_sampling": true, "drop_reasoning_effort_with_tools": true },
               "models": { "claude-sonnet-5-5": "gpt-fake" } },
    "limited": { "base_url": "http://127.0.0.1:$(cat "$work/limited.port")", "protocol": "openai_chat",
                 "messages_path": "/openai/deployments/{model}/chat/completions?api-version=2024-10-21",
                 "auth": { "kind": "header", "header": "api-key", "env": "CHAT_KEY" },
                 "models": { "claude-sonnet-5-5": "gpt-fake" } }
  },
  "routes": { "other": ["azure"] },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
CHAT_KEY=fake-azure-key "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
gateway_pid=$!
while ! grep -q "listening on" "$work/gateway.log"; do sleep 0.1; done

python3 - "$port" "$work" <<'PY'
import http.client
import json
import sys

port, work = int(sys.argv[1]), sys.argv[2]
LONG = "mcp__server__" + "x" * 80
TOOLS = [{"name": "Bash", "description": "run a command", "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}},
         {"name": LONG, "description": "a tool with a very long name", "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}}]
failures = []


def check(label, condition):
    print(("ok   " if condition else "FAIL ") + label)
    if not condition:
        failures.append(label)


def post(payload, model="claude-sonnet-5-5"):
    fields = dict({"model": model, "max_tokens": 64, "stream": True, "temperature": 0.3, "stop_sequences": ["END"], "thinking": {"type": "adaptive"},
                   "output_config": {"effort": "medium"}, "metadata": {"user_id": "u"}, "system": "Be brief.", "tools": TOOLS}, **payload)
    body = json.dumps({key: value for key, value in fields.items() if value is not None})
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("POST", "/anthropic/v1/messages", body, {"content-type": "application/json", "anthropic-beta": "tools-2024", "anthropic-version": "2023-06-01",
                                                                  "x-claude-code-session-id": "sess-e2e", "x-api-key": "client-secret-xyz"})
    reply = connection.getresponse()
    text = reply.read().decode()
    events = [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ")]
    return reply.status, events, text


def get(path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("GET", path)
    return json.loads(connection.getresponse().read())


user = {"role": "user", "content": "run echo tool-ran"}
status, first, _ = post({"messages": [user]})
kinds = [event["type"] for event in first]
check("turn 1 answers 200", status == 200)
starts = [e["content_block"] for e in first if e["type"] == "content_block_start"]
check("turn 1 is one tool_use block with the call id and the original long tool name", len(starts) == 1 and starts[0]["type"] == "tool_use" and starts[0]["id"] == "call_fake" and starts[0]["name"] == LONG)
check("turn 1 input fragments rebuild the command", "".join(e["delta"]["partial_json"] for e in first if e["type"] == "content_block_delta") == '{"command": "echo tool-ran"}')
check("turn 1 stops for the tool", next(e for e in first if e["type"] == "message_delta")["delta"]["stop_reason"] == "tool_use")
check("turn 1 starts with message_start and ends with message_stop", kinds[0] == "message_start" and kinds[-1] == "message_stop")

assistant = {"role": "assistant", "content": [{"type": "tool_use", "id": "call_fake", "name": LONG, "input": {"command": "echo tool-ran"}}]}
result = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "call_fake", "content": "tool-ran"}]}
status, second, _ = post({"messages": [user, assistant, result]})
check("turn 2 carries the final text", "".join(e["delta"].get("text", "") for e in second if e["type"] == "content_block_delta") == "Tool finished: all good")
check("turn 2 ends the turn", next(e for e in second if e["type"] == "message_delta")["delta"]["stop_reason"] == "end_turn")
final_usage = next(e for e in second if e["type"] == "message_delta")["usage"]
check("usage splits input from cache reads", final_usage["input_tokens"] == 20 and final_usage["cache_read_input_tokens"] == 100 and final_usage["output_tokens"] == 9)

logged = [json.loads(line) for line in open(f"{work}/tool.log")]
check("the upstream saw the deployment path with the model filled in and the api-version query", all(r["path"] == "/openai/deployments/gpt-fake/chat/completions?api-version=2024-10-21" for r in logged))
check("the upstream saw the api-key header and no Authorization or Anthropic header",
      all(r["headers"].get("api-key") == "fake-azure-key" and "authorization" not in r["headers"] and "anthropic-beta" not in r["headers"] and "x-api-key" not in r["headers"] for r in logged))
sent = logged[0]["request"]
check("the request uses max_completion_tokens and carries no stop, temperature, top_p or reasoning_effort", sent.get("max_completion_tokens") == 64 and not any(k in sent for k in ("max_tokens", "stop", "temperature", "top_p", "reasoning_effort")))
check("the request streams with usage", sent["stream"] is True and sent["stream_options"] == {"include_usage": True})
check("the long tool name was replaced for the upstream", [t["function"]["name"] for t in sent["tools"]][0] == "Bash" and len(sent["tools"][1]["function"]["name"]) <= 64 and sent["tools"][1]["function"]["name"] != LONG)
second_messages = logged[1]["request"]["messages"]
check("turn 2 history is system, user, assistant with tool_calls, tool, in order", [m["role"] for m in second_messages] == ["system", "user", "assistant", "tool"] and second_messages[3]["tool_call_id"] == "call_fake")
check("the assistant tool call carries the replaced name and JSON arguments", second_messages[2]["tool_calls"][0]["function"]["name"] == sent["tools"][1]["function"]["name"] and json.loads(second_messages[2]["tool_calls"][0]["function"]["arguments"]) == {"command": "echo tool-ran"})

status, _, text = post({"messages": [user], "stream": False})
check("a request that is not streamed is refused with a clear message", status == 400 and "stream must be true" in text)
status, _, text = post({"messages": [user], "tools": [{"type": "advisor_20260301", "name": "advisor"}]})
check("a server tool is refused with the wording Claude Code's recovery recognises", status == 400 and "Input tag 'advisor_20260301'" in text)

stats = get("/llmgate/stats")
check("both streamed turns were counted and nothing needed repair", stats["streamed"] == 2 and all(count == 0 for count in stats["repairs"].values()))
check("dropped fields are counted by name", stats["dropped_fields"].get("metadata") == 2)
lines = [json.loads(line) for line in open(f"{work}/usage.jsonl")]
answered = [line for line in lines if line["status"] == 200]
check("the ledger holds one answered line per streamed turn with input and cache read split",
      len(answered) == 2 and all(line["provider"] == "azure" and line["cache_read"] == 100 and line["input"] == 20 for line in answered))

def last_request():
    return [json.loads(line) for line in open(f"{work}/tool.log")][-1]["request"]


bare = {"tools": None, "output_config": None, "thinking": None}
post(dict(bare, system=[{"type": "text", "text": "Be brief."}], messages=[
    {"role": "user", "content": [{"type": "text", "text": "look"}, {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "QUJD"}},
                                  {"type": "image", "source": {"type": "url", "url": "https://x.test/a.png"}}]},
    {"role": "assistant", "content": [{"type": "thinking", "thinking": "private", "signature": "s"}, {"type": "text", "text": "ok"}, {"type": "text", "text": "second"}]},
    {"role": "system", "content": "extra"}, {"role": "user", "content": "again"}]))
sent = last_request()
parts = sent["messages"][1]["content"]
check("a history with a system block, images, thinking and a system message keeps its order", [m["role"] for m in sent["messages"]] == ["system", "user", "assistant", "system", "user"])
check("images become image_url parts after the text", [p["type"] for p in parts] == ["text", "image_url", "image_url"] and parts[1]["image_url"]["url"] == "data:image/png;base64,QUJD" and parts[2]["image_url"]["url"] == "https://x.test/a.png")
check("assistant text blocks are joined and thinking is dropped", sent["messages"][2]["content"] == "ok\nsecond" and "private" not in json.dumps(sent))

for choice, expected in (({"type": "auto"}, "auto"), ({"type": "none"}, "none"), ({"type": "any", "disable_parallel_tool_use": True}, "required"),
                         ({"type": "tool", "name": "Bash"}, {"type": "function", "function": {"name": "Bash"}})):
    post({"tool_choice": choice, "messages": [user]})
    sent = last_request()
    check(f"tool_choice {choice['type']} maps to {json.dumps(expected)}", sent["tool_choice"] == expected and (sent.get("parallel_tool_calls") is False) == choice.get("disable_parallel_tool_use", False))

for budget, effort in ((3000, "low"), (8000, "medium"), (20000, "high")):
    post(dict(bare, thinking={"type": "enabled", "budget_tokens": budget}, messages=[user]))
    check(f"a thinking budget of {budget} tokens becomes reasoning_effort {effort}", last_request().get("reasoning_effort") == effort)
post(dict(bare, output_config={"effort": "low"}, messages=[user]))
check("a named effort is passed on", last_request().get("reasoning_effort") == "low")
post(dict(bare, thinking={"type": "disabled"}, messages=[user]))
check("disabled thinking sends no effort", "reasoning_effort" not in last_request())

for label, messages, wanted in (("a tool_use block in a user message", [{"role": "user", "content": [{"type": "tool_use", "id": "c", "name": "n", "input": {}}]}], "a user message cannot hold"),
                                ("a tool_use without an id", [{"role": "assistant", "content": [{"type": "tool_use", "name": "n", "input": {}}]}], "tool_use needs an id"),
                                ("an image with no source", [{"role": "user", "content": [{"type": "image", "source": {"type": "file"}}]}], "base64 or url"),
                                ("an image in an assistant message", [{"role": "assistant", "content": [{"type": "image", "source": {"type": "url", "url": "u"}}]}], "an assistant message cannot hold")):
    status, _, text = post(dict(bare, messages=messages))
    check(f"{label} is refused with a 400 that names it", status == 400 and wanted in text)

status, events, _ = post(dict(bare, messages=[{"role": "user", "content": "FAKE_ERROR"}]))
check("an unparsable chunk is skipped and an error chunk becomes an Anthropic error event", events[-1]["type"] == "error" and events[-1]["error"]["type"] == "rate_limit_error" and events[-1]["error"]["message"] == "overloaded")
check("the unparsable chunk is counted", get("/llmgate/stats")["ignored_events"].get("unparsed_chunk") == 1)

status, _, text = post(dict(bare, messages=[{"role": "user", "content": "FAKE_400"}]))
check("an upstream 400 reaches Claude Code with the upstream's own message", status == 400 and json.loads(text)["error"]["type"] == "invalid_request_error" and json.loads(text)["error"]["message"] == "bad field xyz")

limited_status, _, limited = post({"messages": [user]}, model="claude-gpt-fake@limited")
body = json.loads(limited)
check("an upstream rate limit reaches Claude Code as an Anthropic rate_limit_error",
      limited_status == 429 and body["type"] == "error" and body["error"]["type"] == "rate_limit_error" and body["error"]["message"] == "Rate limit reached")
sys.exit(1 if failures else 0)
PY
