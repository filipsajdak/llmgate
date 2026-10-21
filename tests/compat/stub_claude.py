import json
import os
import pathlib
import subprocess
import sys
import urllib.request
import uuid

scenario_dir = pathlib.Path(os.environ["COMPAT_SCENARIO_DIR"])
lib = pathlib.Path(__file__).with_name("lib.sh")
arguments = sys.argv[1:]
session = arguments[arguments.index("--resume") + 1] if "--resume" in arguments else str(uuid.uuid4())
history = [{"role": "user", "content": "hi"}]
if "--resume" in arguments:
    history += [{"role": "assistant", "content": [{"type": "thinking", "thinking": "x", "signature": "from-the-first-provider"}, {"type": "text", "text": "ok"}]},
                {"role": "user", "content": "go on"}]
request = urllib.request.Request(
    os.environ["ANTHROPIC_BASE_URL"] + "/v1/messages",
    data=json.dumps({"model": "claude-sonnet-5-5", "max_tokens": 8, "messages": history}).encode(),
    headers={"content-type": "application/json", "x-api-key": os.environ.get("ANTHROPIC_AUTH_TOKEN", ""), "anthropic-beta": "compat-selftest-2026",
             "x-claude-code-session-id": session},
)
urllib.request.urlopen(request, timeout=20).read()
print(json.dumps({"type": "system", "subtype": "init", "session_id": session}))

events = []
stub = scenario_dir / "stub.sh"
if os.environ.get("COMPAT_STUB_MODE") != "idle" and stub.exists():
    ran = subprocess.run(["sh", "-c", f". {lib}; . {stub}"], capture_output=True, text=True, check=True)
    events = [line for line in ran.stdout.splitlines() if line.strip()]
last_text = ""
for line in events:
    for block in (json.loads(line).get("message") or {}).get("content", []):
        if block.get("type") == "text":
            last_text = block["text"]
if not events and os.environ.get("COMPAT_STUB_MODE") != "idle":
    prompt = arguments[arguments.index("-p") + 1] if "-p" in arguments else ""
    last_text = prompt.split()[-1].strip(".!?") if prompt.split() else ""
for line in events:
    print(line)
print(json.dumps({"type": "result", "is_error": False, "result": last_text}))
