import base64
import http.server
import json
import select
import socket
import ssl
import sys
import threading

pki, ports_file, log_file = sys.argv[1], sys.argv[2], sys.argv[3]
logged = open(log_file, "a", buffering=1)
ORIGIN_NAME = "origin.test"
STAND_INS = (ORIGIN_NAME, "192.0.2.7")
MODEL_LIST = {"data": [{"type": "model", "id": "proxied-model", "display_name": "Proxied model"}]}


def log(line):
    logged.write(line + "\n")


class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps(MODEL_LIST).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def tls_server(cert, key, handler):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(f"{pki}/{cert}.pem", f"{pki}/{key}.key")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def pump(first, second):
    sockets = [first, second]
    try:
        while True:
            ready, _, _ = select.select(sockets, [], [], 30)
            if not ready:
                return
            for each in ready:
                data = each.recv(65536)
                if not data:
                    return
                (second if each is first else first).sendall(data)
    except OSError:
        pass
    finally:
        for each in sockets:
            try:
                each.close()
            except OSError:
                pass


def make_proxy(name, origin_port, needs_login=False, intercepts=False, denies=False):
    class Proxy(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def reply(self, status, text, extra=None):
            self.send_response(status, text)
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True

        def authorised(self):
            header = self.headers.get("Proxy-Authorization")
            return header is not None and base64.b64decode(header.split()[1]).decode() == "user:pass"

        def do_CONNECT(self):
            log(f"{name} CONNECT {self.path} auth={'present' if 'Proxy-Authorization' in self.headers else 'absent'}")
            if denies:
                return self.reply(403, "Forbidden")
            if needs_login and not self.authorised():
                return self.reply(407, "Proxy Authentication Required", {"Proxy-Authenticate": 'Basic realm="test"'})
            host, _, port = self.path.rpartition(":")
            target = ("127.0.0.1", origin_port) if host in STAND_INS else (host, int(port))
            if intercepts:
                return self.intercept(target)
            upstream = socket.create_connection(target)
            self.wfile.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            self.wfile.flush()
            pump(self.connection, upstream)
            self.close_connection = True

        def intercept(self, target):
            self.wfile.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            self.wfile.flush()
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(f"{pki}/corp.pem", f"{pki}/corp.key")
            try:
                client = context.wrap_socket(self.connection, server_side=True)
            except (ssl.SSLError, OSError) as failure:
                log(f"{name} client refused the certificate: {type(failure).__name__}")
                self.close_connection = True
                return
            request = client.recv(65536)
            log(f"{name} saw {request.split(b' ')[0].decode()}")
            trusting = ssl.create_default_context(cafile=f"{pki}/testca.pem")
            upstream = trusting.wrap_socket(socket.create_connection(target), server_hostname=ORIGIN_NAME)
            upstream.sendall(request)
            pump(client, upstream)
            self.close_connection = True

        def log_message(self, *args):
            pass

    return Proxy


def start(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


origin = start(tls_server("origin", "origin", Origin))
ports = {"origin": origin}
for key, options in {"plain": {}, "login": {"needs_login": True}, "intercept": {"intercepts": True}, "deny": {"denies": True}}.items():
    ports[key] = start(http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_proxy(key, origin, **options)))
with open(ports_file, "w") as written:
    json.dump(ports, written)
threading.Event().wait()
