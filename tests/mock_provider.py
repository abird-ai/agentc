#!/usr/bin/env python3
"""Mock Anthropic Messages provider for the agentc end-to-end test.

Serves POST /v1/messages with a canned SSE stream:
  - first request  -> a tool_use call reading src/base/mem.c
  - second request -> text "done" with end_turn
Writes the chosen port to stdout so the caller can start agentc against it.
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def sse(events):
    out = b""
    for name, data in events:
        out += f"event: {name}\ndata: {json.dumps(data)}\n\n".encode()
    return out


def tool_turn(model):
    return sse([
        ("message_start", {"type": "message_start", "message": {
            "id": "msg_1", "type": "message", "role": "assistant", "content": [],
            "model": model, "usage": {"input_tokens": 12, "output_tokens": 1}}}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "tool_use", "id": "toolu_1",
                                                   "name": "read", "input": {}}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "input_json_delta",
                                           "partial_json": "{\"path\": \"src/base/mem.c\"}"}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"},
                           "usage": {"output_tokens": 5}}),
        ("message_stop", {"type": "message_stop"}),
    ])


def trunc_turn(model):
    """tool_turn cut short at max_tokens: PARTIAL tool args, never executed."""
    return sse([
        ("message_start", {"type": "message_start", "message": {
            "id": "msg_1", "type": "message", "role": "assistant", "content": [],
            "model": model, "usage": {"input_tokens": 12, "output_tokens": 1}}}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "tool_use", "id": "toolu_1",
                                                   "name": "read", "input": {}}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "input_json_delta",
                                           "partial_json": "{\"path\": \"src/base/mem.c\""}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "max_tokens"},
                           "usage": {"output_tokens": 5}}),
        ("message_stop", {"type": "message_stop"}),
    ])


def grep_turn(model):
    """First turn when the prompt contains GREP: call the grep tool, so e2e can
    exercise the tool-engine backend (external rg vs built-in) end to end."""
    return sse([
        ("message_start", {"type": "message_start", "message": {
            "id": "msg_grep", "type": "message", "role": "assistant", "content": [],
            "model": model, "usage": {"input_tokens": 12, "output_tokens": 1}}}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "tool_use", "id": "toolu_grep",
                                                   "name": "grep", "input": {}}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "input_json_delta",
                                           "partial_json": "{\"pattern\": \"AGENTC_E2E_GREP\"}"}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"},
                           "usage": {"output_tokens": 5}}),
        ("message_stop", {"type": "message_stop"}),
    ])


def bg_turn(model):
    """First turn when the prompt contains BG: start a background bash process
    that writes a sentinel. The tool returns the actor id immediately and the
    turn finishes; the sentinel proves the background actor kept running."""
    return sse([
        ("message_start", {"type": "message_start", "message": {
            "id": "msg_bg", "type": "message", "role": "assistant", "content": [],
            "model": model, "usage": {"input_tokens": 12, "output_tokens": 1}}}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "tool_use", "id": "toolu_bg",
                                                   "name": "bash", "input": {}}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "input_json_delta",
                                           "partial_json": "{\"command\": \"echo BG-OK > build/e2e-bg-ok\", \"background\": true}"}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"},
                           "usage": {"output_tokens": 5}}),
        ("message_stop", {"type": "message_stop"}),
    ])


def write_turn(model):
    """First turn when the prompt contains APPROVE: call `write` with a path
    outside the workspace so the trust gate asks for approval."""
    return sse([
        ("message_start", {"type": "message_start", "message": {
            "id": "msg_appr", "type": "message", "role": "assistant", "content": [],
            "model": model, "usage": {"input_tokens": 12, "output_tokens": 1}}}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "tool_use", "id": "toolu_appr",
                                                   "name": "write", "input": {}}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "input_json_delta",
                                           "partial_json": "{\"path\": \"/etc/agentc-e2e-approval.txt\", \"content\": \"approved\"}"}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"},
                           "usage": {"output_tokens": 5}}),
        ("message_stop", {"type": "message_stop"}),
    ])


def text_turn(model):
    return sse([
        ("message_start", {"type": "message_start", "message": {
            "id": "msg_2", "type": "message", "role": "assistant", "content": [],
            "model": model, "usage": {"input_tokens": 40, "output_tokens": 1}}}),
        ("content_block_start", {"type": "content_block_start", "index": 0,
                                 "content_block": {"type": "text", "text": ""}}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                 "delta": {"type": "text_delta", "text": "done"}}),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn"},
                           "usage": {"output_tokens": 2}}),
        ("message_stop", {"type": "message_stop"}),
    ])


def has_tool_result(body):
    for m in body.get("messages", []):
        if not isinstance(m, dict):
            continue
        content = m.get("content")
        if isinstance(content, list):
            for c in content:
                if isinstance(c, dict) and c.get("type") == "tool_result":
                    return True
    return False


def newest_user_text(body):
    """The most recent user message that carries real text (a tool_result-only
    user message is skipped, so a marker in the original prompt is still seen on
    the post-tool step)."""
    for m in reversed(body.get("messages", [])):
        if not isinstance(m, dict) or m.get("role") != "user":
            continue
        content = m.get("content")
        if isinstance(content, str):
            if content:
                return content
            continue
        if isinstance(content, list):
            text = "".join(c.get("text", "") for c in content
                           if isinstance(c, dict) and c.get("type") == "text")
            if text:
                return text
    return ""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        raw = self.rfile.read(n) if n else b"{}"
        raw_text = raw.decode("utf-8", "replace")
        try:
            body = json.loads(raw)
            malformed = False
        except Exception:
            body = {}
            malformed = True
        # A partial tool call can make the replayed request body unparseable;
        # the raw marker still tells us the transcript carried a closure.
        if has_tool_result(body) or (malformed and "tool_result" in raw_text):
            payload = text_turn(body.get("model", "mock"))
            # A SLOW prompt stalls the post-tool (second) model step, so a caller
            # can hold two sessions both mid-run and cancel one of them. The
            # first step stays the fast tool turn (it has no tool_result yet),
            # yields at the post-tool safe point, then step two stalls.
            paced = "SLOW" in newest_user_text(body) or (malformed and "SLOW" in raw_text)
        elif "PACE" in newest_user_text(body) or (malformed and "PACE" in raw_text):
            # A paced stream: the same text turn, but the handler stalls between
            # SSE events so a front end can abort the in-flight read (M6 steer).
            payload = text_turn(body.get("model", "mock"))
            paced = True
        elif "BG" in newest_user_text(body) or (malformed and "BG" in raw_text):
            payload = bg_turn(body.get("model", "mock"))
            paced = False
        elif "TRUNC" in newest_user_text(body) or (malformed and "TRUNC" in raw_text):
            payload = trunc_turn(body.get("model", "mock"))
            paced = False
        elif "APPROVE" in newest_user_text(body):
            payload = write_turn(body.get("model", "mock"))
            paced = False
        elif "GREP" in newest_user_text(body):
            payload = grep_turn(body.get("model", "mock"))
            paced = False
        else:
            payload = tool_turn(body.get("model", "mock"))
            paced = False
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("content-length", str(len(payload)))
        self.send_header("connection", "close")
        self.end_headers()
        if paced:
            # Deliver the text delta quickly, then stall for a long time before
            # the terminal events. A front end can only abort during the stall
            # if the HTTP read observes the cancel predicate; waiting for the
            # next chunk would take 20 s.
            try:
                parts = [p for p in payload.split(b"\n\n") if p]
                for i, piece in enumerate(parts):
                    self.wfile.write(piece + b"\n\n")
                    self.wfile.flush()
                    time.sleep(20.0 if i == 2 else 0.05)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.wfile.write(payload)
        self.close_connection = True


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)
    server.serve_forever()
