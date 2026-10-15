#!/usr/bin/env python3
"""Tiny deterministic HTTP server for tests/net.sh and tests/mcp.sh. Prints its
port, then serves:
   GET  /hello    -> "hello world\\n"
   GET  /sse      -> three server-sent events
   GET  /chunked  -> chunked body "hello world\\n" with an extension + trailer
   GET  /eof      -> HTTP/1.0 body delimited by connection close
   GET  /redirect -> 302 to /hello (the client must not follow it)
   POST /echo     -> echoes the request body as application/json

MCP Streamable-HTTP test routes (tests/mcp.sh):
   POST /mcp      -> initialize (echoes Mcp-Session-Id), notifications/
                     initialized (202), tools/list (v1 or v2 depending on the
                     current tools version) and tools/call
   GET  /mcp      -> Accept: text/event-stream opens an SSE stream; the first
                     stream after the v1 sync sleeps briefly, emits
                     notifications/tools/list_changed, flips tools to v2 and
                     keeps the stream open
   POST /mcp405   -> same MCP POST handling
   GET  /mcp405   -> 405 Method Not Allowed (the client must disable the stream
                     after exactly one attempt)
   POST /mcp404s  -> same MCP POST handling
   GET  /mcp404s  -> 404 answered with Content-Type: text/event-stream and an
                     SSE error-page body (the client must treat it as transient
                     and never route the body as notifications)
When $MCP_HTTPD_LOG is set every request line plus the Accept /
Mcp-Session-Id / Last-Event-ID headers is appended there.

Usage: tests/httpd.py [--tls CERT KEY]
"""
import http.server
import json
import os
import socketserver
import ssl
import sys
import threading
import time

LOG_PATH = os.environ.get("MCP_HTTPD_LOG", "")
_log_lock = threading.Lock()


def log_line(text):
    if not LOG_PATH:
        return
    with _log_lock:
        try:
            with open(LOG_PATH, "a") as f:
                f.write(text + "\n")
        except OSError:
            pass


class McpState:
    def __init__(self):
        self.lock = threading.Lock()
        self.tools_version = 1
        self.stream_started = False
        self.stop = threading.Event()

    def tools(self):
        desc = "echo v2" if self.tools_version >= 2 else "echo v1"
        return [{
            "name": "echo",
            "description": desc,
            "inputSchema": {"type": "object",
                            "properties": {"text": {"type": "string"}},
                            "required": ["text"]},
            "annotations": {"readOnlyHint": True},
        }]


MCP = McpState()


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

    def _req_headers(self):
        return "accept=%s session=%s last=%s" % (
            self.headers.get("Accept", "-"),
            self.headers.get("Mcp-Session-Id", "-"),
            self.headers.get("Last-Event-ID", "-"))

    # --------------------------------------------------------------- MCP POST
    def _mcp_post(self, body):
        try:
            msg = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            msg = {}
        if not isinstance(msg, dict):
            msg = {}
        method = msg.get("method")
        mid = msg.get("id")
        log_line("POST %s method=%s %s" % (self.path, method, self._req_headers()))
        if method == "initialize":
            payload = {"jsonrpc": "2.0", "id": mid, "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "httpd-mcp", "version": "1.0"}}}
            raw = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Mcp-Session-Id", "sess-1")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw)
            return
        if method == "notifications/initialized":
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        if method == "tools/list":
            with MCP.lock:
                tools = MCP.tools()
            payload = {"jsonrpc": "2.0", "id": mid, "result": {"tools": tools}}
            self._send(200, "application/json", json.dumps(payload).encode())
            return
        if method == "tools/call":
            payload = {"jsonrpc": "2.0", "id": mid, "result": {
                "content": [{"type": "text", "text": "called"}]}}
            self._send(200, "application/json", json.dumps(payload).encode())
            return
        if mid is not None:
            payload = {"jsonrpc": "2.0", "id": mid,
                       "error": {"code": -32601, "message": "method not found"}}
            self._send(200, "application/json", json.dumps(payload).encode())
        else:
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()

    # ----------------------------------------------------------- MCP GET stream
    def _mcp_get_stream(self):
        log_line("GET %s %s" % (self.path, self._req_headers()))
        if "text/event-stream" not in self.headers.get("Accept", ""):
            self._send(405, "text/plain", b"expect text/event-stream\n")
            return
        with MCP.lock:
            first = not MCP.stream_started
            MCP.stream_started = True
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b": connected\n\n")
        self.wfile.flush()
        if first:
            time.sleep(0.3)
            with MCP.lock:
                MCP.tools_version = 2
            note = json.dumps({"jsonrpc": "2.0",
                               "method": "notifications/tools/list_changed"})
            try:
                self.wfile.write(("id: 1\nevent: message\ndata: %s\n\n" % note).encode())
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                return
            # Close the first stream after the notification: the client keeps
            # the list_changed and reconnects with Last-Event-ID (which the
            # next GET then carries).
            return
        # Later streams stay open until the client disconnects or the server is
        # told to stop. ThreadingTCPServer gives each request its own thread, so
        # a blocked stream never starves the concurrent POSTs.
        while not MCP.stop.is_set():
            time.sleep(0.1)

    def do_GET(self):
        if self.path == "/mcp":
            self._mcp_get_stream()
            return
        if self.path == "/mcp405":
            log_line("GET %s %s" % (self.path, self._req_headers()))
            self._send(405, "text/plain", b"method not allowed\n")
            return
        if self.path == "/mcp404s":
            log_line("GET %s %s" % (self.path, self._req_headers()))
            body = (b": error\n\nevent: message\ndata: "
                    b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                    b"tools/list_changed\"}\n\n")
            self._send(404, "text/event-stream", body)
            return
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
        body = self.rfile.read(n) if n else b""
        if self.path in ("/mcp", "/mcp405", "/mcp404s"):
            self._mcp_post(body)
            return
        self._send(200, "application/json", body)


class ThreadingServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    cert = key = None
    if "--tls" in sys.argv:
        i = sys.argv.index("--tls")
        cert, key = sys.argv[i + 1], sys.argv[i + 2]

    with ThreadingServer(("127.0.0.1", 0), H) as srv:
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        print(srv.server_address[1], flush=True)
        srv.serve_forever()
