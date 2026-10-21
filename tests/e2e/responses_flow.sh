#!/bin/sh
set -eu
PYTHONPATH="$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/build/host/llmgate}
work=$(mktemp -d)
trap 'kill $tool_pid $limited_pid $gateway_pid 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

FAKE_REQUIRE_AUTHORIZATION="Bearer fake-plan-token" python3 "$root/tests/e2e/fake_responses.py" "$work/tool.port" "$work/tool.log" tool &
tool_pid=$!
python3 "$root/tests/e2e/fake_responses.py" "$work/limited.port" "$work/limited.log" rate_limited &
limited_pid=$!
while [ ! -s "$work/tool.port" ] || [ ! -s "$work/limited.port" ]; do sleep 0.1; done

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$work/config.jsonc" <<JSON
{
  "listen": "127.0.0.1:$port",
  "providers": {
    "codex": { "base_url": "http://127.0.0.1:$(cat "$work/tool.port")", "protocol": "openai_responses", "static_token_env": "RESPONSES_TOKEN",
               "models": { "claude-sonnet-5-5": "gpt-fake" } },
    "limited": { "base_url": "http://127.0.0.1:$(cat "$work/limited.port")", "protocol": "openai_responses", "static_token_env": "RESPONSES_TOKEN",
                 "models": { "claude-sonnet-5-5": "gpt-fake" } }
  },
  "routes": { "other": ["codex"] },
  "usage": { "ledger_file": "$work/usage.jsonl" }
}
JSON
RESPONSES_TOKEN=fake-plan-token "$binary" --config "$work/config.jsonc" > "$work/gateway.log" 2>&1 &
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
    body = json.dumps(dict({"model": model, "max_tokens": 64, "stream": True, "temperature": 0.3, "tools": TOOLS}, **payload))
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("POST", "/anthropic/v1/messages", body, {"content-type": "application/json", "anthropic-beta": "tools-2024", "anthropic-version": "2023-06-01",
                                                                  "x-claude-code-session-id": "sess-e2e"})
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
check("turn 1 is one tool_use block with the call id and name",
      any(e.get("content_block", {}).get("type") == "tool_use" and e["content_block"]["id"] == "call_fake" and e["content_block"]["name"] == "Bash" for e in first))
check("turn 1 input fragments rebuild the command", "".join(e["delta"]["partial_json"] for e in first if e["type"] == "content_block_delta") == '{"command": "echo tool-ran"}')
check("turn 1 stops for the tool", next(e for e in first if e["type"] == "message_delta")["delta"]["stop_reason"] == "tool_use")
check("turn 1 starts with message_start and ends with message_stop", kinds[0] == "message_start" and kinds[-1] == "message_stop")

assistant = {"role": "assistant", "content": [{"type": "tool_use", "id": "call_fake", "name": "Bash", "input": {"command": "echo tool-ran"}}]}
result = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "call_fake", "content": "tool-ran"}]}
status, second, _ = post({"messages": [user, assistant, result]})
check("turn 2 carries the final text", "".join(e["delta"].get("text", "") for e in second if e["type"] == "content_block_delta") == "Tool finished: all good")
check("turn 2 ends the turn", next(e for e in second if e["type"] == "message_delta")["delta"]["stop_reason"] == "end_turn")
final_usage = next(e for e in second if e["type"] == "message_delta")["usage"]
check("usage splits input from cache reads", final_usage["input_tokens"] == 20 and final_usage["cache_read_input_tokens"] == 100 and final_usage["output_tokens"] == 9)

logged = [json.loads(line) for line in open(f"{work}/tool.log")]
check("the upstream saw the Responses path and the plan token", all(r["path"] == "/responses" and r["headers"]["authorization"] == "Bearer fake-plan-token" for r in logged))
check("the upstream saw no Anthropic header", all("anthropic-beta" not in r["headers"] and "anthropic-version" not in r["headers"] for r in logged))
second_input = logged[1]["request"]["input"]
check("turn 2 history is a function call and its output, in order",
      [item["type"] for item in second_input] == ["message", "function_call", "function_call_output"] and second_input[2]["call_id"] == "call_fake")
check("the prompt cache key is a hash of the session, not the session", logged[0]["request"].get("prompt_cache_key") not in (None, "sess-e2e"))

status, _, text = post({"messages": [user], "stream": False})
check("a request that is not streamed is refused with a clear message", status == 400 and "stream must be true" in text)
status, _, text = post({"messages": [user], "tools": [{"type": "advisor_20260301", "name": "advisor"}]})
check("a server tool is refused with the wording Claude Code's recovery recognises", status == 400 and "Input tag 'advisor_20260301'" in text)

stats = get("/llmgate/stats")
check("both streamed turns were counted and nothing needed repair", stats["streamed"] == 2 and all(count == 0 for count in stats["repairs"].values()))
check("dropped fields are counted by name", stats["dropped_fields"].get("max_tokens") == 2 and stats["dropped_fields"].get("temperature") == 2)
check("the ignored events are counted (none unknown here)", stats["ignored_events"] == {})
lines = [json.loads(line) for line in open(f"{work}/usage.jsonl")]
answered = [line for line in lines if line["status"] == 200]
check("the ledger holds one answered line per streamed turn with input and cache read split",
      len(answered) == 2 and all(line["provider"] == "codex" and line["cache_read"] == 100 and line["input"] == 20 for line in answered))
check("the two refused requests are in the ledger as 400", sorted(line["status"] for line in lines if line["status"] != 200) == [400, 400])

pinned_status, _, pinned = post({"messages": [user]}, model="claude-gpt-fake@limited")
body = json.loads(pinned.split("\n\n")[0]) if pinned.startswith("{") else json.loads(pinned)
check("an upstream rate limit reaches Claude Code as an Anthropic rate_limit_error",
      pinned_status == 429 and body["type"] == "error" and body["error"]["type"] == "rate_limit_error" and body["error"]["message"] == "Rate limit reached")
sys.exit(1 if failures else 0)
PY
