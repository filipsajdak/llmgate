import base64
import hashlib
import json
import os
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import fast_http  # noqa: F401

PORT_FILE, LOG = sys.argv[1], sys.argv[2]
EXPIRES_IN = int(os.environ.get("FAKE_AUTH_EXPIRES_IN", "3600"))
SCOPE = os.environ.get("FAKE_AUTH_SCOPE", "openid offline_access chatgpt.tokens.use.direct")
ISSUED_CLIENT = "issued_e2e"
pending = {}
state = {"issued": 0, "refresh": "refresh-0"}


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def jwt(claims):
    return b64(json.dumps({"alg": "RS256"}).encode()) + "." + b64(json.dumps(claims).encode()) + ".sig"


def log(entry):
    with open(LOG, "a") as handle:
        handle.write(json.dumps(entry) + "\n")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=None):
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(raw)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path != "/api/accounts/authorize":
            return self.reply(404, {})
        query = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        required = ["response_type", "client_id", "redirect_uri", "scope", "state", "nonce", "code_challenge", "code_challenge_method", "resource", "ext_agent_host_id"]
        missing = [name for name in required if name not in query]
        if missing or query["code_challenge_method"] != "S256" or "chatgpt.tokens.use.direct" not in query["scope"].split():
            log({"authorize": "refused", "missing": missing})
            return self.reply(400, {"error": "invalid_request", "missing": missing})
        issued = ISSUED_CLIENT if query["client_id"] == "dynamic_agent_client" else query["client_id"]
        code = "code-" + b64(os.urandom(6))
        pending[code] = dict(query, issued=issued)
        log({"authorize": "ok", "client_id": query["client_id"], "agent_name_hint": query.get("agent_name_hint"), "host": query["ext_agent_host_id"]})
        target = query["redirect_uri"] + "?" + urllib.parse.urlencode({"code": code, "state": query["state"], "client_id": issued})
        self.send_response(302)
        self.send_header("location", target)
        self.send_header("content-length", "0")
        self.end_headers()

    def do_POST(self):
        form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("content-length", 0))).decode()).items()}
        if self.path != "/api/accounts/oauth/token":
            return self.reply(404, {})
        if form.get("grant_type") == "authorization_code":
            grant = pending.pop(form.get("code", ""), None)
            challenge = b64(hashlib.sha256(form.get("code_verifier", "").encode()).digest())
            if grant is None or challenge != grant["code_challenge"] or form.get("redirect_uri") != grant["redirect_uri"] or form.get("client_id") != grant["issued"]:
                log({"token": "refused", "grant": "authorization_code"})
                return self.reply(400, {"error": "invalid_grant"})
            audience, nonce = grant["issued"], grant["nonce"]
        elif form.get("grant_type") == "refresh_token":
            if form.get("refresh_token") != state["refresh"] or form.get("client_id") != ISSUED_CLIENT:
                log({"token": "refused", "grant": "refresh_token"})
                return self.reply(400, {"error": "invalid_grant"})
            audience, nonce = ISSUED_CLIENT, "-"
        else:
            return self.reply(400, {"error": "unsupported_grant_type"})
        state["issued"] += 1
        state["refresh"] = f"refresh-{state['issued']}"
        issuer = "http://" + self.headers["host"]
        claims = {"iss": issuer, "aud": audience, "nonce": nonce, "exp": int(time.time()) + 600, "sub": "e2e-user"}
        log({"token": "ok", "grant": form["grant_type"], "n": state["issued"], "iss": issuer, "aud": audience, "host_header": self.headers["host"]})
        self.reply(200, {"access_token": f"access-{state['issued']}", "refresh_token": state["refresh"], "id_token": jwt(claims), "token_type": "Bearer",
                         "expires_in": EXPIRES_IN, "scope": SCOPE})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
with open(PORT_FILE, "w") as handle:
    handle.write(str(server.server_address[1]))
server.serve_forever()
