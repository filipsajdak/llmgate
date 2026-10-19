import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import fast_http  # noqa: F401

PORT_FILE = sys.argv[1]
COPILOT_API = sys.argv[2]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, body, status=200):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        self.rfile.read(int(self.headers.get("content-length", 0)))
        if self.path == "/login/device/code":
            self.reply({"device_code": "dc", "user_code": "ABCD-1234", "verification_uri": "http://example.test/device", "expires_in": 60, "interval": 0})
        else:
            self.reply({"access_token": "gho_rehearsal_token"})

    def do_GET(self):
        if self.path == "/copilot_internal/v2/token":
            now = int(time.time())
            self.reply({"token": "tid=rehearsal", "expires_at": now + 3600, "refresh_in": 1500, "endpoints": {"api": COPILOT_API}})
        elif self.path == "/copilot_internal/user":
            self.reply({"copilot_plan": "business", "access_type_sku": "copilot_for_business_seat", "quota_reset_date": "2099-01-01"})
        else:
            self.reply({}, 404)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(PORT_FILE, "w") as handle:
    handle.write(str(server.server_address[1]))
server.serve_forever()
