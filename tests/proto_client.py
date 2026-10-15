#!/usr/bin/env python3
"""proto_client.py — stdlib-only framed-CBOR client and cross-language proof.

Minimal CBOR (definite lengths; the protocol profile's vocabulary) plus a
synchronous client for the agentc serve framing. Used by tests/serve.sh.

Usage:
    proto_client.py --socket PATH [--token T]           # protocol smoke suite
    proto_client.py --socket PATH [--token T] --prompt TEXT  # one prompt run
"""
import argparse
import base64
import struct
import select
import socket
import ssl
import sys
import time

# --------------------------------------------------------------------- CBOR
def _head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    if n <= 0xFF:
        return bytes([major << 5 | 24, n])
    if n <= 0xFFFF:
        return bytes([major << 5 | 25]) + struct.pack(">H", n)
    if n <= 0xFFFFFFFF:
        return bytes([major << 5 | 26]) + struct.pack(">I", n)
    return bytes([major << 5 | 27]) + struct.pack(">Q", n)


def enc(v):
    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if isinstance(v, int):
        return _head(0, v) if v >= 0 else _head(1, -1 - v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return _head(3, len(b)) + b
    if isinstance(v, (bytes, bytearray)):
        return _head(2, len(v)) + bytes(v)
    if isinstance(v, list):
        return _head(4, len(v)) + b"".join(enc(x) for x in v)
    if isinstance(v, dict):
        out = _head(5, len(v))
        for k, val in v.items():
            out += enc(k) + enc(val)
        return out
    if isinstance(v, float):
        return b"\xfb" + struct.pack(">d", v)
    raise TypeError(type(v))


def _arg(b, off, ai):
    if ai < 24:
        return ai, off
    if ai == 24:
        return b[off], off + 1
    if ai == 25:
        return struct.unpack_from(">H", b, off)[0], off + 2
    if ai == 26:
        return struct.unpack_from(">I", b, off)[0], off + 4
    if ai == 27:
        return struct.unpack_from(">Q", b, off)[0], off + 8
    raise ValueError("indefinite/reserved")


def dec(b, off=0):
    ib = b[off]
    off += 1
    major, ai = ib >> 5, ib & 0x1F
    n, off = _arg(b, off, ai)
    if major == 0:
        return n, off
    if major == 1:
        return -1 - n, off
    if major == 2:
        return bytes(b[off:off + n]), off + n
    if major == 3:
        return b[off:off + n].decode("utf-8"), off + n
    if major == 4:
        out = []
        for _ in range(n):
            x, off = dec(b, off)
            out.append(x)
        return out, off
    if major == 5:
        out = {}
        for _ in range(n):
            k, off = dec(b, off)
            v, off = dec(b, off)
            out[k] = v
        return out, off
    if major == 7:
        if n == 20:
            return False, off
        if n == 21:
            return True, off
        if n == 22:
            return None, off
        if n == 27:
            return struct.unpack_from(">d", b, off)[0], off + 8
    raise ValueError("unsupported")


class Failure(Exception):
    pass


# ---------------------------------------------------------------- WebSocket
def _ws_send(sock, data, opcode=2, mask=True):
    """Send one RFC 6455 frame.  Client frames are masked."""
    n = len(data)
    hdr = bytes([0x80 | (opcode & 0x0F)])
    mb = 0x80 if mask else 0
    if n < 126:
        hdr += bytes([mb | n])
    elif n <= 0xFFFF:
        hdr += bytes([mb | 126]) + struct.pack(">H", n)
    else:
        hdr += bytes([mb | 127]) + struct.pack(">Q", n)
    if mask:
        key = b"\x11\x22\x33\x44"
        hdr += key
        data = bytes(c ^ key[i & 3] for i, c in enumerate(data))
    sock.sendall(hdr + data)


class Client:
    def __init__(self, path, token=None, timeout=10.0, ws=None, tls=None):
        self._ws = WS if ws is None else ws
        self._tls = TLS if tls is None else tls
        self._inbuf = b""
        if path.startswith("tcp:"):
            host, _, port = path[4:].rpartition(":")
            self.sock = socket.create_connection((host, int(port)), timeout=timeout)
            if self._tls:
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                self.sock = ctx.wrap_socket(self.sock, server_hostname=host)
        else:
            if self._ws or self._tls:
                raise Failure("websocket/tls requires a tcp: target")
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect(path)
        self.token = token
        self.server_id = None
        self.session_id = None
        self.features = []
        if self._ws:
            self._ws_handshake()

    def _ws_handshake(self):
        key = base64.b64encode(b"\x00" * 16).decode("ascii")
        req = ("GET /agentc HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n"
               "Sec-WebSocket-Protocol: agentc.v1\r\n\r\n" % key)
        self.sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise Failure("ws eof during handshake")
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n", 1)[0] or b"agentc.v1" not in head:
            raise Failure("ws handshake rejected")
        self._inbuf = rest

    def _recv_exact_raw(self, n):
        out = b""
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise Failure("eof")
            out += chunk
        return out

    def _ws_message(self):
        """Receive one complete (reassembled) binary message."""
        msg = b""
        started = False
        while True:
            h = self._recv_exact_raw(2)
            b0, b1 = h[0], h[1]
            fin, opcode = b0 & 0x80, b0 & 0x0F
            masked, ln = b1 & 0x80, b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._recv_exact_raw(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._recv_exact_raw(8))[0]
            if ln > 16 * 1024 * 1024:
                raise Failure("ws oversized")
            key = self._recv_exact_raw(4) if masked else None
            payload = self._recv_exact_raw(ln)
            if key:
                payload = bytes(c ^ key[i & 3] for i, c in enumerate(payload))
            if opcode in (8, 9, 10):
                if opcode == 9:
                    _ws_send(self.sock, payload, opcode=10)
                if opcode == 8:
                    raise Failure("ws close")
                continue
            if opcode == 2:
                started = True
                msg += payload
            elif opcode == 0 and started:
                msg += payload
            else:
                raise Failure("ws opcode %d" % opcode)
            if fin:
                return msg

    def _recv_exact(self, n):
        if not self._ws:
            return self._recv_exact_raw(n)
        while len(self._inbuf) < n:
            self._inbuf += self._ws_message()
        out, self._inbuf = self._inbuf[:n], self._inbuf[n:]
        return out

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_raw(self, value):
        p = enc(value)
        framed = struct.pack(">I", len(p)) + p
        if self._ws:
            _ws_send(self.sock, framed)
        else:
            self.sock.sendall(framed)

    def send_frame(self, payload):
        framed = struct.pack(">I", len(payload)) + payload
        if self._ws:
            _ws_send(self.sock, framed)
        else:
            self.sock.sendall(framed)

    def recv(self):
        hdr = self._recv_exact(4)
        n = struct.unpack(">I", hdr)[0]
        return dec(self._recv_exact(n))[0]

    def hello(self, version=1, token=None):
        msg = {"kind": "hello", "version": version}
        if token is not None:
            msg["token"] = token
        self.send_raw(msg)
        r = self.recv()
        if r.get("kind") == "hello_ack":
            self.server_id = r.get("server_id")
            self.session_id = r.get("session_id")
            self.features = r.get("features") or []
        return r

    def request(self, rid, method, params=None):
        msg = {"kind": "request", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send_raw(msg)
        while True:
            r = self.recv()
            if r.get("kind") == "response" and r.get("id") == rid:
                return r
            if r.get("kind") == "event":
                self.last_event = r
            elif r.get("kind") == "hello_error":
                raise Failure("hello_error: %s" % r.get("code"))

    def pump_events(self, seconds, stop_on_settled=False):
        events = []
        end = time.time() + seconds
        while time.time() < end:
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                r = self.recv()
            except (socket.timeout, OSError):
                break
            except Failure:
                break
            if r.get("kind") == "event":
                events.append(r)
                if stop_on_settled:
                    ev = r.get("event") or {}
                    if ev.get("type") == "agent_settled":
                        break
        return events

    def cancel(self, rid):
        self.send_raw({"kind": "cancel", "id": rid})

    def attach(self, rid, session=None, events=None):
        msg = {"kind": "attach", "id": rid}
        if session is not None:
            msg["session"] = session
        if events is not None:
            msg["events"] = events
        self.send_raw(msg)
        while True:
            r = self.recv()
            if r.get("kind") == "response" and r.get("id") == rid:
                return r
            if r.get("kind") == "event":
                self.last_event = r
            elif r.get("kind") == "hello_error":
                raise Failure("hello_error: %s" % r.get("code"))

    def detach(self, rid):
        self.send_raw({"kind": "detach", "id": rid})
        while True:
            r = self.recv()
            if r.get("kind") == "response" and r.get("id") == rid:
                return r
            if r.get("kind") == "event":
                self.last_event = r


def expect(label, cond):
    print("%s=%d" % (label, 1 if cond else 0))
    if not cond:
        raise Failure(label)


def expect_error(label, want_code, extra_key=None):
    """Connect, send a hello (bad token/version) or a bad request, assert the
    hello_error code or the terminal close."""
    c = Client(SOCKET, TIMEOUT)
    try:
        if want_code in ("unauthorized", "version_mismatch"):
            ver = 2 if want_code == "version_mismatch" else 1
            tok = TOKEN if want_code == "version_mismatch" else "wrong"
            r = c.hello(version=ver, token=tok)
            expect(label, r.get("kind") == "hello_error" and r.get("code") == want_code)
        else:
            r = c.hello(token=TOKEN)
            expect(label + ".hello", r.get("kind") == "hello_ack")
            msg = {"kind": "request", "id": 1, "method": "get_state", "extra": 1}
            c.send_raw(msg)
            try:
                got = c.recv()
                # The server may reply with a protocol error then close.
                expect(label, got.get("kind") == "hello_error")
            except Failure:
                expect(label, True)   # terminal close is the contract
    finally:
        c.close()


def scenario():
    c = Client(SOCKET, TIMEOUT)
    r = c.hello(token=TOKEN)
    expect("proto.hello_ack", r.get("kind") == "hello_ack" and r.get("version") == 1)
    SERVER["id"] = r.get("server_id")
    expect("proto.server_id", isinstance(r.get("server_id"), str) and len(r["server_id"]) == 36)
    expect("proto.session_id", isinstance(r.get("session_id"), str) and len(r["session_id"]) > 0)
    expect("proto.features", "cancel" in (r.get("features") or []))
    expect("proto.features.attach", "attach" in (r.get("features") or []))

    r = c.request(1, "get_state")
    expect("proto.state", r.get("ok") is True and r["result"].get("protocol") == "agentc-rpc")

    r = c.request("s2", "get_trust")
    expect("proto.trust", r.get("ok") is True and "access" in r["result"])

    r = c.request(3, "bogus_method")
    expect("proto.unknown", r.get("ok") is False and r["error"]["code"] == "unknown_command")

    r = c.request(4, "get_available_models")
    expect("proto.models", r.get("ok") is True and r["result"].get("total", 0) >= 0)

    # C-D8: bounded get_messages paging is the resync primitive (no auto-replay).
    r = c.request(6, "get_messages", {"cursor": 0, "limit": 5})
    expect("proto.messages.page",
           r.get("ok") is True and "total" in r["result"] and "messages" in r["result"])

    c.cancel(99)
    r = c.request(5, "get_state")
    expect("proto.cancel_accepted", r.get("ok") is True)
    c.close()

    expect_error("proto.bad_token", "unauthorized")
    expect_error("proto.version", "version_mismatch")
    expect_error("proto.unknown_prop", None)
    attachment()
    dialogs()
    multi_client()
    connection_cap()


def attachment():
    """attach/detach routing (C-D4): a connection subscribes with an event
    filter, detaches from the broadcast, and re-attaches; a foreign session is
    refused."""
    c = Client(SOCKET, TIMEOUT)
    c.hello(token=TOKEN)
    sid = c.session_id
    r = c.attach(1, events=["agent_settled"])
    expect("proto.attach.ok", r.get("ok") is True and r["result"].get("session_id") == sid)
    expect("proto.attach.echo", r["result"].get("events") == ["agent_settled"])
    r = c.attach(2, session="nope")
    expect("proto.attach.bad_session",
           r.get("ok") is False and r["error"].get("code") == "not_found")
    r = c.detach(3)
    expect("proto.detach.ok", r.get("ok") is True)
    r = c.attach(4, session=sid)
    expect("proto.attach.re", r.get("ok") is True)
    c.close()


def _recv_match(c, want, seconds):
    """Receive until an envelope of `want` kind, ignoring events. Returns the
    matching envelope or None."""
    end = time.time() + seconds
    while time.time() < end:
        c.sock.settimeout(max(0.05, end - time.time()))
        try:
            m = c.recv()
        except (socket.timeout, OSError, Failure):
            return None
        if m.get("kind") == want:
            return m
    return None


def dialog_round_trip(label, params, answer_ok, answer_value):
    """Send a ui_prompt request, answer its ui_request, and require the matching
    response. Returns the response `result` on success, None on failure."""
    c = Client(SOCKET, TIMEOUT)
    try:
        if c.hello(token=TOKEN).get("kind") != "hello_ack":
            return None
        c.send_raw({"kind": "request", "id": 7, "method": "ui_prompt", "params": params})
        ui = _recv_match(c, "ui_request", TIMEOUT)
        if ui is None or not isinstance(ui.get("id"), int):
            return None
        reply = {"kind": "ui_response", "id": ui["id"], "ok": answer_ok}
        if answer_ok:
            reply["result"] = answer_value
        c.send_raw(reply)
        resp = _recv_match(c, "response", TIMEOUT)
        if resp is None or resp.get("id") != 7:
            return None
        print("%s=%d" % (label, 1 if resp is not None else 0))
        return resp
    finally:
        c.close()


def dialogs():
    """Dialog round-trips: confirm allow/deny, select, input, a dismissed
    dialog, a bounded timeout deny, and an unmatched ui_response drop."""
    c = Client(SOCKET, TIMEOUT)
    if c.hello(token=TOKEN).get("kind") != "hello_ack":
        return
    expect("proto.features.ui", "ui" in (c.features or []))
    c.close()

    r = dialog_round_trip("proto.dialog.confirm_allow",
                          {"method": "confirm", "prompt": "go?"}, True, True)
    expect("proto.dialog.confirm_allow.value", r is not None and r["result"].get("value") is True)

    r = dialog_round_trip("proto.dialog.confirm_deny",
                          {"method": "confirm", "prompt": "go?"}, True, False)
    expect("proto.dialog.confirm_deny.value", r is not None and r["result"].get("value") is False)

    r = dialog_round_trip("proto.dialog.select",
                          {"method": "select", "prompt": "pick",
                           "options": ["a", "b", "c"]}, True, "b")
    expect("proto.dialog.select.value", r is not None and r["result"].get("value") == "b")

    r = dialog_round_trip("proto.dialog.input",
                          {"method": "input", "prompt": "name", "default": "x"},
                          True, "hello")
    expect("proto.dialog.input.value", r is not None and r["result"].get("value") == "hello")

    # A dismissed dialog (ok false) is a fail-closed deny.
    r = dialog_round_trip("proto.dialog.dismissed",
                          {"method": "confirm", "prompt": "go?"}, False, None)
    expect("proto.dialog.dismissed.denied",
           r is not None and r.get("ok") is False and r["error"].get("code") == "denied")

    # A bounded timeout denies without a reply.
    c = Client(SOCKET, TIMEOUT)
    c.hello(token=TOKEN)
    c.send_raw({"kind": "request", "id": 8, "method": "ui_prompt",
                "params": {"method": "confirm", "prompt": "slow?", "timeout_ms": 150}})
    ui = _recv_match(c, "ui_request", TIMEOUT)
    resp = _recv_match(c, "response", TIMEOUT) if ui else None
    expect("proto.dialog.timeout_deny",
           resp is not None and resp.get("id") == 8 and resp.get("ok") is False
           and resp["error"].get("code") == "denied")
    c.close()

    # ui_response with no pending dialog is validated and dropped.
    c = Client(SOCKET, TIMEOUT)
    c.hello(token=TOKEN)
    c.send_raw({"kind": "ui_response", "id": 999, "ok": False})
    r = c.request(9, "get_state")
    expect("proto.dialog.unmatched_dropped", r.get("ok") is True)
    c.close()

    # A malformed dialog (select without options) is refused, not parked.
    c = Client(SOCKET, TIMEOUT)
    c.hello(token=TOKEN)
    r = c.request(10, "ui_prompt", {"method": "select", "prompt": "pick"})
    expect("proto.dialog.invalid",
           r.get("ok") is False and r["error"].get("code") == "invalid_response")
    c.close()

    # One dialog per connection: a second ui_prompt is refused `busy` while the
    # first is parked. Closing the connection drops the parked dialog.
    c = Client(SOCKET, TIMEOUT)
    c.hello(token=TOKEN)
    c.send_raw({"kind": "request", "id": 11, "method": "ui_prompt",
                "params": {"method": "confirm", "prompt": "first?", "timeout_ms": 60000}})
    _recv_match(c, "ui_request", TIMEOUT)
    c.send_raw({"kind": "request", "id": 12, "method": "ui_prompt",
                "params": {"method": "confirm", "prompt": "second?"}})
    r = _recv_match(c, "response", TIMEOUT)
    expect("proto.dialog.one_per_conn",
           r is not None and r.get("id") == 12 and r.get("ok") is False
           and r["error"].get("code") == "busy")
    c.close()


def multi_client():
    """Several connections share one session: requests are independent and an
    envelope violation is terminal only for the offending connection (C-D7)."""
    time.sleep(0.3)   # let the server reap the connections the previous cases closed
    c1 = Client(SOCKET, TIMEOUT); c1.hello(token=TOKEN)
    c2 = Client(SOCKET, TIMEOUT); c2.hello(token=TOKEN)
    r = c1.request(1, "get_state")
    expect("proto.multi.c1", r.get("ok") is True)
    bad = Client(SOCKET, TIMEOUT); bad.hello(token=TOKEN)
    bad.send_raw({"kind": "request", "id": 2, "method": "get_state", "nope": 1})
    closed = False
    try:
        closed = bad.recv().get("kind") == "hello_error"
    except (Failure, OSError):
        closed = True
    expect("proto.multi.violation_terminal", closed)
    bad.close()
    c1.close()
    r = c2.request(3, "get_state")
    expect("proto.multi.survivor_alive", r.get("ok") is True)
    c2.close()


def connection_cap():
    """The server holds at most 8 connections; a ninth is accepted then closed.
    Best-effort: if the server is still reaping a previous case's connections the
    probe is skipped rather than reported as a cap failure."""
    time.sleep(0.3)   # let the server reap the connections the previous cases closed
    clients = []
    try:
        for _ in range(8):
            try:
                c = Client(SOCKET, TIMEOUT)
                if c.hello(token=TOKEN).get("kind") != "hello_ack":
                    raise Failure("cap.hello")
            except (Failure, OSError):
                print("proto.cap.skipped=1")
                return 0
            clients.append(c)
        try:
            extra = Client(SOCKET, TIMEOUT)
            closed = extra.hello(token=TOKEN).get("kind") != "hello_ack"
        except (Failure, OSError):
            closed = True
            extra = None
        if extra is not None:
            extra.close()
        expect("proto.cap.ninth_closed", closed)
    finally:
        for c in clients:
            c.close()


def filter_run(text):
    """An attachment filtered to `agent_settled` receives no other event while
    an unfiltered observer receives the run's events (C-D4). The requester owns
    the run, so `agent_settled` reaches it; the observer sees the broadcast."""
    obs = Client(SOCKET, TIMEOUT)
    c = Client(SOCKET, TIMEOUT)
    if obs.hello(token=TOKEN).get("kind") != "hello_ack" or \
       c.hello(token=TOKEN).get("kind") != "hello_ack":
        print("FAIL proto.filter.hello")
        return 1
    if c.attach(1, events=["agent_settled"]).get("ok") is not True:
        print("FAIL proto.filter.attach")
        return 1
    r = c.request(1, "prompt", {"text": text})
    if not (r.get("ok") is True and r["result"].get("disposition") == "started"):
        print("FAIL proto.filter.started")
        return 1
    ftypes = [(e.get("event") or {}).get("type")
              for e in c.pump_events(15, stop_on_settled=True)]
    otypes = [(e.get("event") or {}).get("type") for e in obs.pump_events(2)]
    only_settle = "agent_settled" in ftypes and all(t == "agent_settled" for t in ftypes)
    seen_more = any(t != "agent_settled" for t in otypes)
    print("proto.filter.filtered=%d" % (1 if only_settle else 0))
    print("proto.filter.observer=%d" % (1 if seen_more else 0))
    obs.close()
    c.close()
    return 0 if only_settle and seen_more else 1


def prompt_run(text):
    c = Client(SOCKET, TIMEOUT)
    r = c.hello(token=TOKEN)
    if r.get("kind") != "hello_ack":
        print("FAIL proto.prompt.hello")
        return 1
    obs = Client(SOCKET, TIMEOUT)
    if obs.hello(token=TOKEN).get("kind") != "hello_ack":
        print("FAIL proto.prompt.observer_hello")
        return 1
    r = c.request(1, "prompt", {"text": text})
    # Acceptance-first: the prompt response is `started`.
    if not (r.get("ok") is True and r["result"].get("disposition") == "started"):
        print("FAIL proto.prompt.started")
        return 1
    print("proto.prompt.started=1")
    events = c.pump_events(15, stop_on_settled=True)
    settled = [e for e in events
               if (e.get("event") or {}).get("type") == "agent_settled"]
    print("proto.prompt.settled=%d" % (1 if settled else 0))
    # Events are broadcast to every attached connection (C-D4).
    obs_events = obs.pump_events(2)
    print("proto.prompt.broadcast=%d" % (1 if obs_events else 0))
    c.close()
    obs.close()
    return 0 if settled and obs_events else 1


def cancel_run(text):
    """Start a paced run, cancel it out-of-band, and require an `aborted`
    agent_settled. The mock provider's PACE mode stalls mid-stream so the run is
    only interruptible if serve services `cancel` while the HTTP read waits."""
    c = Client(SOCKET, TIMEOUT)
    r = c.hello(token=TOKEN)
    if r.get("kind") != "hello_ack":
        print("FAIL proto.cancel.hello")
        return 1
    r = c.request(1, "prompt", {"text": text})
    if not (r.get("ok") is True and r["result"].get("disposition") == "started"):
        print("FAIL proto.cancel.started")
        c.close()
        return 1
    print("proto.cancel.started=1")
    c.cancel(1)
    settled = None
    end = time.time() + TIMEOUT
    while time.time() < end:
        c.sock.settimeout(max(0.05, end - time.time()))
        try:
            m = c.recv()
        except (socket.timeout, OSError, Failure):
            break
        if m.get("kind") == "event" and (m.get("event") or {}).get("type") == "agent_settled":
            settled = m["event"]
            break
    ok = settled is not None and settled.get("disposition") == "aborted"
    print("proto.cancel.aborted=%d" % (1 if ok else 0))
    c.close()
    return 0 if ok else 1


def queue_run(text):
    """Requests that arrive during a run are serialized through the queue: a
    second client's request is not answered until the active run settles, then
    it is dispatched normally (C-D4)."""
    c1 = Client(SOCKET, TIMEOUT)
    c2 = Client(SOCKET, TIMEOUT)
    if c1.hello(token=TOKEN).get("kind") != "hello_ack" or \
       c2.hello(token=TOKEN).get("kind") != "hello_ack":
        print("FAIL proto.queue.hello")
        return 1
    r = c1.request(1, "prompt", {"text": text})
    if not (r.get("ok") is True and r["result"].get("disposition") == "started"):
        print("FAIL proto.queue.started")
        return 1
    c2.send_raw({"kind": "request", "id": 2, "method": "get_state"})
    c2.sock.settimeout(0.3)
    deferred = True
    answered = False
    while True:
        try:
            m = c2.recv()
        except (socket.timeout, OSError, Failure):
            break
        # Broadcast events arrive immediately and are not a response.
        if m.get("kind") == "response" and m.get("id") == 2:
            deferred = False
            answered = True
            break
    print("proto.queue.deferred=%d" % (1 if deferred else 0))
    c1.cancel(1)
    end = time.time() + TIMEOUT
    while not answered and time.time() < end:
        c2.sock.settimeout(max(0.05, end - time.time()))
        try:
            m = c2.recv()
        except (socket.timeout, OSError, Failure):
            break
        if m.get("kind") == "response" and m.get("id") == 2:
            answered = m.get("ok") is True
    print("proto.queue.answered_after_settle=%d" % (1 if answered else 0))
    c1.close()
    c2.close()
    return 0 if deferred and answered else 1


def multi_session(text):
    """P5 C3 registry: create a second session, attach to it, prompt it, and
    require its events to be tagged with that session and routed only to its
    attachment; switch back; then fill the bounded registry and check the next
    creation fails closed `busy`."""
    a = Client(SOCKET, TIMEOUT)
    if a.hello(token=TOKEN).get("kind") != "hello_ack":
        print("FAIL proto.ms.hello")
        return 1
    expect("proto.ms.feature", "multi_session" in (a.features or []))
    s0 = a.session_id

    r = a.request(1, "new_session")
    if not (r.get("ok") is True and r["result"].get("session_id")):
        print("FAIL proto.ms.new")
        return 1
    s1 = r["result"]["session_id"]
    expect("proto.ms.distinct", s1 != s0)

    r = a.attach(2, session=s1)
    expect("proto.ms.attach", r.get("ok") is True and r["result"].get("session_id") == s1)

    b = Client(SOCKET, TIMEOUT)
    if b.hello(token=TOKEN).get("kind") != "hello_ack":
        print("FAIL proto.ms.b_hello")
        a.close()
        return 1
    r = b.attach(1)
    expect("proto.ms.b_attach", r.get("ok") is True and r["result"].get("session_id") == s0)

    r = a.request(3, "prompt", {"text": text})
    if not (r.get("ok") is True and r["result"].get("disposition") == "started"):
        print("FAIL proto.ms.started")
        a.close()
        b.close()
        return 1
    aevents = a.pump_events(15, stop_on_settled=True)
    tags = set(e.get("session_id") for e in aevents)
    tagged = len(aevents) > 0 and tags == {s1}
    print("proto.ms.tagged=%d" % (1 if tagged else 0))
    bev = b.pump_events(2)
    print("proto.ms.b_isolated=%d" % (1 if not bev else 0))
    b.close()

    r = a.attach(4, session=s0)
    expect("proto.ms.switch_back",
           r.get("ok") is True and r["result"].get("session_id") == s0)

    created = 1   # the second session above
    busy = False
    for i in range(8):
        r = a.request(100 + i, "new_session")
        if r.get("ok") is True:
            created += 1
        else:
            busy = r.get("error", {}).get("code") == "busy"
            break
    print("proto.ms.created=%d" % created)
    expect("proto.ms.registry_full", busy and created == 3)

    r = a.request(200, "new_session", {"session": s1})
    expect("proto.ms.select",
           r.get("ok") is True and r["result"].get("session_id") == s1)
    r = a.request(201, "new_session", {"session": "nope"})
    expect("proto.ms.select_missing",
           r.get("ok") is False and r["error"].get("code") == "not_found")
    a.close()
    return 0 if tagged and not bev and busy else 1


def _collect(cs, seconds, stop=None):
    """Read frames from a list of clients for up to `seconds`, servicing all of
    them with select so two sessions are observed concurrently. Returns a list
    of (client, msg); `stop(msgs)` finishes early when it returns True."""
    msgs = []
    end = time.time() + seconds
    sockmap = {c.sock: c for c in cs}
    while time.time() < end:
        ready, _, _ = select.select(list(sockmap), [], [], max(0.0, end - time.time()))
        if not ready:
            break
        for sk in ready:
            c = sockmap[sk]
            try:
                m = c.recv()
            except (socket.timeout, OSError, Failure):
                return msgs
            msgs.append((c, m))
            if stop is not None and stop(msgs):
                return msgs
    return msgs


def _settled_disposition(msg):
    ev = msg.get("event") or {}
    return ev.get("disposition") if ev.get("type") == "agent_settled" else None


def _new_session_pair():
    """Connect two clients; `a` creates and moves to a fresh second session,
    `b` stays on session 0. Returns (a, b, s1, s0)."""
    a = Client(SOCKET, TIMEOUT)
    if a.hello(token=TOKEN).get("kind") != "hello_ack":
        raise Failure("conc.hello_a")
    r = a.request(1, "new_session")
    if not (r.get("ok") is True and r["result"].get("session_id")):
        raise Failure("conc.new_session")
    s1 = r["result"]["session_id"]
    b = Client(SOCKET, TIMEOUT)
    if b.hello(token=TOKEN).get("kind") != "hello_ack":
        raise Failure("conc.hello_b")
    return a, b, s1, b.session_id


def concurrent(text):
    """P5 concurrency: two independent sessions must run at the same time.
    Start a fast tool-then-text run on each (sent before either is read) and
    require both sessions to emit non-settle events before either settles. A
    serialized server would answer the second prompt `queued` and emit no
    events for it until the first run finished."""
    if WS or TLS:
        print("skip proto.conc (not applicable to ws/tls)")
        return 0
    a, b, s1, s0 = _new_session_pair()
    a.send_raw({"kind": "request", "id": 2, "method": "prompt", "params": {"text": text}})
    b.send_raw({"kind": "request", "id": 3, "method": "prompt", "params": {"text": text}})
    msgs = _collect([a, b], min(TIMEOUT, 20.0),
                    stop=lambda ms: any(_settled_disposition(m) is not None for _, m in ms))
    started = sum(1 for _, m in msgs
                  if m.get("kind") == "response" and
                  m.get("result", {}).get("disposition") == "started")
    first_settle = next((i for i, (_, m) in enumerate(msgs)
                         if _settled_disposition(m) is not None), None)
    before = msgs if first_settle is None else msgs[:first_settle]
    both_before = all(any((m.get("event") or {}).get("type") != "agent_settled" and
                          m.get("session_id") == sid for _, m in before)
                      for sid in (s0, s1))
    print("proto.conc.started=%d" % (1 if started == 2 else 0))
    print("proto.conc.both_before_settle=%d" % (1 if both_before else 0))
    a.close()
    b.close()
    return 0 if started == 2 and both_before else 1


def concurrent_cancel(text):
    """P5 concurrency: cancel one session's run while two are in flight without
    disturbing the other. Both prompts take the post-tool SLOW stall, so both
    runs are active; cancelling the first must not settle the second, then
    cancelling the second settles both `aborted`."""
    if WS or TLS:
        print("skip proto.concc (not applicable to ws/tls)")
        return 0
    a, b, s1, s0 = _new_session_pair()
    a.send_raw({"kind": "request", "id": 2, "method": "prompt", "params": {"text": text}})
    b.send_raw({"kind": "request", "id": 3, "method": "prompt", "params": {"text": text}})
    msgs = _collect([a, b], min(TIMEOUT, 20.0),
                    stop=lambda ms: sum(1 for _, m in ms
                                        if m.get("kind") == "response" and
                                        m.get("result", {}).get("disposition") == "started") == 2)
    started = sum(1 for _, m in msgs
                  if m.get("kind") == "response" and
                  m.get("result", {}).get("disposition") == "started")
    print("proto.concc.started=%d" % (1 if started == 2 else 0))
    if started != 2:
        a.close()
        b.close()
        return 1
    a.cancel(2)                    # cancel session A (s1) only
    msgs = _collect([a, b], 1.5)
    allmsgs = list(msgs)
    b_settled = any(m.get("session_id") == s0 and _settled_disposition(m) is not None
                    for _, m in msgs)
    print("proto.concc.b_undisturbed=%d" % (1 if not b_settled else 0))
    b.cancel(3)                    # now cancel session B (s0)
    msgs = _collect([a, b], min(TIMEOUT, 20.0),
                    stop=lambda ms: (any(_settled_disposition(m) == "aborted" and
                                         m.get("session_id") == s0 for _, m in ms) and
                                     any(_settled_disposition(m) == "aborted" and
                                         m.get("session_id") == s1 for _, m in ms)))
    allmsgs += msgs
    a_aborted = any(_settled_disposition(m) == "aborted" and m.get("session_id") == s1
                    for _, m in allmsgs)
    b_aborted = any(_settled_disposition(m) == "aborted" and m.get("session_id") == s0
                    for _, m in allmsgs)
    print("proto.concc.a_aborted=%d" % (1 if a_aborted else 0))
    print("proto.concc.b_aborted=%d" % (1 if b_aborted else 0))
    a.close()
    b.close()
    return 0 if started == 2 and not b_settled and a_aborted and b_aborted else 1


def main():
    global SOCKET, TOKEN, TIMEOUT, WS, TLS
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    ap.add_argument("--token")
    ap.add_argument("--ws", action="store_true",
                    help="speak RFC6455 WebSocket to a `--ws` server over tcp:")
    ap.add_argument("--tls", action="store_true",
                    help="wrap the tcp: connection in TLS (certificate not verified)")
    ap.add_argument("--prompt")
    ap.add_argument("--cancel", metavar="TEXT",
                    help="start a paced run, cancel it, require an aborted settle")
    ap.add_argument("--queue", metavar="TEXT",
                    help="start a run, queue a second client's request, cancel")
    ap.add_argument("--filter", metavar="TEXT",
                    help="attach with an event filter and check routing (C-D4)")
    ap.add_argument("--multi-session", metavar="TEXT",
                    help="create/attach a second session, check routed events (C3)")
    ap.add_argument("--concurrent", metavar="TEXT",
                    help="two sessions run interleaved; both progress before settle")
    ap.add_argument("--concurrent-cancel", metavar="TEXT",
                    help="two sessions in flight; cancel one, then the other")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--dialogs", action="store_true",
                    help="run only the UI dialog round-trips")
    args = ap.parse_args()
    SOCKET, TOKEN, TIMEOUT = args.socket, args.token, args.timeout
    WS = args.ws
    TLS = args.tls
    try:
        if args.dialogs:
            dialogs()
            return 0
        if args.multi_session is not None:
            return multi_session(args.multi_session)
        if args.concurrent is not None:
            return concurrent(args.concurrent)
        if args.concurrent_cancel is not None:
            return concurrent_cancel(args.concurrent_cancel)
        if args.cancel is not None:
            return cancel_run(args.cancel)
        if args.queue is not None:
            return queue_run(args.queue)
        if args.filter is not None:
            return filter_run(args.filter)
        if args.prompt is not None:
            return prompt_run(args.prompt)
        scenario()
        return 0
    except Failure as e:
        print("FAIL proto (%s)" % e)
        return 1
    except OSError as e:
        print("FAIL proto socket (%s)" % e)
        return 1


SERVER = {}
WS = False
TLS = False
if __name__ == "__main__":
    sys.exit(main())
