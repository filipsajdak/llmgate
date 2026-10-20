import json
import os
import pathlib
import subprocess
import sys
import urllib.request

scenario_dir = pathlib.Path(os.environ["COMPAT_SCENARIO_DIR"])
lib = pathlib.Path(__file__).with_name("lib.sh")
request = urllib.request.Request(
    os.environ["ANTHROPIC_BASE_URL"] + "/v1/messages",
    data=json.dumps({"model": "sonnet", "max_tokens": 8, "messages": [{"role": "user", "content": "hi"}]}).encode(),
    headers={"content-type": "application/json", "x-api-key": os.environ.get("ANTHROPIC_AUTH_TOKEN", ""), "anthropic-beta": "compat-selftest-2026"},
)
urllib.request.urlopen(request, timeout=20).read()

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
for line in events:
    print(line)
print(json.dumps({"type": "result", "is_error": False, "result": last_text}))
