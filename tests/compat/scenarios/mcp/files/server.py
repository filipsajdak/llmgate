import json
import sys

ANSWER = "gateway-compat-42"
for line in sys.stdin:
    try:
        message = json.loads(line)
    except ValueError:
        continue
    method, ident = message.get("method"), message.get("id")
    if ident is None:
        continue
    if method == "initialize":
        result = {"protocolVersion": message["params"].get("protocolVersion", "2025-03-26"), "capabilities": {"tools": {}}, "serverInfo": {"name": "compat", "version": "1"}}
    elif method == "tools/list":
        result = {"tools": [{"name": "lookup", "description": "Returns the compatibility answer.", "inputSchema": {"type": "object", "properties": {}}}]}
    elif method == "tools/call":
        result = {"content": [{"type": "text", "text": ANSWER}]}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": ident, "result": result}), flush=True)
