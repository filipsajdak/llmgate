# Helpers for check.sh and stub.sh. TRANSCRIPT is the stream-json output of the claude run.
transcript_has_tool() {
  python3 - "$TRANSCRIPT" "$1" <<'PY'
import json, sys
wanted = sys.argv[2]
for line in open(sys.argv[1]):
    try:
        event = json.loads(line)
    except ValueError:
        continue
    content = (event.get("message") or {}).get("content")
    if event.get("type") == "assistant" and isinstance(content, list):
        if any(block.get("type") == "tool_use" and block.get("name") == wanted for block in content):
            sys.exit(0)
sys.exit(1)
PY
}

transcript_has_block() {
  python3 - "$TRANSCRIPT" "$1" <<'PY'
import json, sys
wanted = sys.argv[2]
for line in open(sys.argv[1]):
    try:
        event = json.loads(line)
    except ValueError:
        continue
    content = (event.get("message") or {}).get("content")
    if event.get("type") == "assistant" and isinstance(content, list):
        if any(block.get("type") == wanted for block in content):
            sys.exit(0)
sys.exit(1)
PY
}

transcript_result_contains() {
  python3 - "$TRANSCRIPT" "$1" <<'PY'
import json, sys
wanted = sys.argv[2].lower()
for line in open(sys.argv[1]):
    try:
        event = json.loads(line)
    except ValueError:
        continue
    if event.get("type") == "result" and not event.get("is_error") and wanted in str(event.get("result", "")).lower():
        sys.exit(0)
sys.exit(1)
PY
}

emit_assistant_text() { python3 -c 'import json, sys; print(json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": sys.argv[1]}]}}))' "$1"; }
emit_assistant_tool() { python3 -c 'import json, sys; print(json.dumps({"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "toolu_stub", "name": sys.argv[1], "input": {}}]}}))' "$1"; }
emit_assistant_block() { python3 -c 'import json, sys; print(json.dumps({"type": "assistant", "message": {"content": [{"type": sys.argv[1]}]}}))' "$1"; }
