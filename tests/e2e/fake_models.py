import http.server
import json
import sys

LISTING = {
    "object": "list",
    "data": [
        {"id": "up-on", "capabilities": {"family": "up-on", "tokenizer": "o200k_base", "limits": {"max_context_window_tokens": 300000, "max_prompt_tokens": 200000, "max_output_tokens": 100000}}, "policy": {"state": "enabled"}, "supported_endpoints": ["/responses"]},
        {"id": "up-off", "capabilities": {"family": "up-off", "limits": {"max_context_window_tokens": 264000, "max_prompt_tokens": 200000, "max_output_tokens": 64000}}, "policy": {"state": "disabled"}},
        {"id": "up-chat", "capabilities": {"family": "up-chat"}, "policy": {"state": "enabled"}, "supported_endpoints": ["/chat/completions"]},
        {"id": "up-embed", "capabilities": {"family": "up-embed"}, "policy": {"state": "enabled"}, "supported_endpoints": ["/embeddings"]},
    ],
}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/models":
            self.send_response(404)
            self.end_headers()
            return
        if self.headers.get("Authorization") != "Bearer " + sys.argv[3]:
            self.send_response(401)
            self.end_headers()
            return
        body = json.dumps(LISTING).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
open(sys.argv[1], "w").write(str(server.server_port))
server.serve_forever()
