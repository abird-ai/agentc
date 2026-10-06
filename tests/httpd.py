#!/usr/bin/env python3
"""Tiny deterministic HTTP server for tests/net.sh. Prints its port, then serves:
   GET  /hello    -> "hello world\\n"
   GET  /sse      -> three server-sent events
   GET  /chunked  -> chunked body "hello world\\n" with an extension + trailer
   GET  /eof      -> HTTP/1.0 body delimited by connection close
   GET  /redirect -> 302 to /hello (the client must not follow it)
   POST /echo     -> echoes the request body as application/json

Usage: tests/httpd.py [--tls CERT KEY]
"""
import http.server
import socketserver
import ssl
import sys


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/sse":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            for name, data in (("delta", "he"), ("delta", "llo"), ("done", "[DONE]")):
                self.wfile.write(f"event: {name}\ndata: {data}\n\n".encode())
                self.wfile.flush()
            return
        if self.path == "/chunked":
            self.protocol_version = "HTTP/1.1"
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Content-Type", "text/plain")
            self.send_header("Connection", "close")
            self.end_headers()
            for chunk in (b"hello", b" ", b"world\n"):
                self.wfile.write(b"%x;x=1\r\n" % len(chunk))
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: yes\r\n\r\n")
            return
        if self.path == "/eof":
            self.protocol_version = "HTTP/1.0"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"eof-delimited\n")
            return
        if self.path == "/redirect":
            self._send(302, "text/plain", b"")
            return
        self._send(200, "text/plain", b"hello world\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        self._send(200, "application/json", body)


if __name__ == "__main__":
    cert = key = None
    if "--tls" in sys.argv:
        i = sys.argv.index("--tls")
        cert, key = sys.argv[i + 1], sys.argv[i + 2]

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", 0), H) as srv:
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        print(srv.server_address[1], flush=True)
        srv.serve_forever()
