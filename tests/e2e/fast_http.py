import http.server
import socketserver


def server_bind_without_reverse_lookup(self):
    socketserver.TCPServer.server_bind(self)
    self.server_name = "localhost"
    self.server_port = self.server_address[1]


http.server.HTTPServer.server_bind = server_bind_without_reverse_lookup
