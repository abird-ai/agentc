#!/usr/bin/env python3
"""Tiny stdio MCP server for tests/mcp.sh.

Newline-delimited JSON-RPC 2.0:
  initialize                    -> protocolVersion 2024-11-05 + serverInfo and
                                   capabilities selected by $MCP_MOCK_CAPS:
                                     tools (default) -> {"tools":{}}
                                     prompts         -> {"prompts":{"listChanged":true}}
                                     resources       -> {"resources":{"listChanged":true}}
                                     all             -> tools + prompts
                                     all-res         -> tools + prompts + resources
                                     none            -> {}
                                     absent          -> no capabilities key
                                     malformed       -> "nope"
                                   With MCP_MOCK_PROMPT_NOTIFY=1 the server also
                                   emits notifications/prompts/list_changed right
                                   after initialize; MCP_MOCK_RESOURCE_NOTIFY=1
                                   emits notifications/resources/list_changed.
  notifications/initialized     -> no reply
  tools/list                    -> paginated; before `trigger` runs:
                                   page 1: echo, bigschema ; page 2: fail,
                                   badschema, trigger
                                   after `trigger` runs (which also emits
                                   notifications/tools/list_changed):
                                   page 1: echo (changed description), fresh ;
                                   page 2: bigschema, badschema
  tools/call echo               -> text from arguments.text, prefixed with
                                   $MCP_GREETING when set, + structuredContent
  tools/call fail               -> isError: true, text "boom"
  tools/call trigger            -> emits the list_changed notification, flips
                                   the tool list, returns "mutated"
  prompts/list                 -> paginated; before `prompts/get
                                   trigger` runs: page 1: greet, summary,
                                   image, fail (nextCursor pp2); page 2: big,
                                   trigger, "collide a", "collide_a". After
                                   trigger: page 1: greet (v2), fresh, image,
                                   fail; page 2: big, trigger, "collide a",
                                   "collide_a".
  prompts/get greet             -> arguments name/tone -> one or two text blocks
  prompts/get summary           -> one text block with the count argument
  prompts/get image             -> text + image + text (non-text is skipped)
  prompts/get fail              -> JSON-RPC error
  prompts/get big               -> text over AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES
  prompts/get trigger           -> emits notifications/prompts/list_changed,
                                   flips the prompt list, returns "mutated"
  resources/list               -> paginated; page 1: a.txt, trigger
                                   (nextCursor rp2); page 2: blob.bin, an
                                   over-long-uri entry the client must skip.
                                   After file:///trigger is read: page 1: a.txt
                                   (v2), trigger; page 2: blob.bin.
  resources/templates/list     -> paginated; page 1: file:///{path}
                                   (nextCursor tp2); page 2: db://{table}/{id}
  resources/read a.txt         -> two text blocks (alpha, beta)
  resources/read blob.bin      -> a blob entry (binary; client rejects)
  resources/read big.txt       -> text over AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES
  resources/read fail          -> JSON-RPC error
  resources/read trigger       -> emits notifications/resources/list_changed,
                                   flips the resource list, returns "mutated"
  anything else                 -> JSON-RPC error
With MCP_MOCK_IDLE_NOTIFY_MS=N (N>0), after every tools/list response the
server restarts an N-millisecond idle timer; when it fires with no request
arriving the server pushes notifications/tools/list_changed and flips the tool
list (idle-drain test).
Exits when stdin closes, unless MCP_MOCK_IGNORE_SIGTERM=1: then it ignores
SIGTERM and keeps running after stdin EOF, so only the client's SIGKILL
escalation can stop it (tests/mcp.sh stubborn_shutdown).
Every request line is appended to $MCP_MOCK_LOG when set (gating checks).
"""
import json
import os
import signal
import sys
import threading
import time

GREETING = os.environ.get("MCP_GREETING", "")
IGNORE_SIGTERM = os.environ.get("MCP_MOCK_IGNORE_SIGTERM") == "1"
CAPS = os.environ.get("MCP_MOCK_CAPS", "tools")
PROMPT_NOTIFY = os.environ.get("MCP_MOCK_PROMPT_NOTIFY") == "1"
RESOURCE_NOTIFY = os.environ.get("MCP_MOCK_RESOURCE_NOTIFY") == "1"
# after the initial tools/list sync, wait this many idle milliseconds
# and then push notifications/tools/list_changed on the server's own initiative
# (no request in flight), flipping the tool list. 0 disables the mode.
IDLE_NOTIFY_MS = int(os.environ.get("MCP_MOCK_IDLE_NOTIFY_MS") or 0)
LOG = os.environ.get("MCP_MOCK_LOG", "")
if IGNORE_SIGTERM:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

# Flipped by the `trigger` tool; changes the tools/list answer.
MUTATED = False
# Flipped by `prompts/get trigger`; changes the prompts/list answer.
PROMPTS_MUTATED = False
# Flipped by reading file:///trigger; changes the resources/list answer.
RESOURCES_MUTATED = False

# One write lock for stdout: the idle timer thread and the request loop both
# emit JSON-RPC lines, and two interleaved writes would corrupt a line.
_write_lock = threading.Lock()


def emit(msg):
    with _write_lock:
        sys.stdout.write(json.dumps(msg) + "\n")
        sys.stdout.flush()


def reply(msg):
    emit(msg)


def notify(method):
    emit({"jsonrpc": "2.0", "method": method})


_idle_timer = None


def arm_idle():
    """(Re)arm the idle list_changed push after a tools/list response.

    Cancelling and restarting on every page means the notification is only
    emitted after the whole sync has gone quiet, i.e. from an actually idle
    server.
    """
    global _idle_timer
    if IDLE_NOTIFY_MS <= 0 or MUTATED:
        return
    if _idle_timer is not None:
        _idle_timer.cancel()

    def fire():
        global MUTATED
        MUTATED = True
        notify("notifications/tools/list_changed")

    _idle_timer = threading.Timer(IDLE_NOTIFY_MS / 1000.0, fire)
    _idle_timer.daemon = True
    _idle_timer.start()


def result(mid, payload):
    reply({"jsonrpc": "2.0", "id": mid, "result": payload})


def capabilities():
    if CAPS == "prompts":
        return {"prompts": {"listChanged": True}}
    if CAPS == "resources":
        return {"resources": {"listChanged": True}}
    if CAPS == "all":
        return {"tools": {}, "prompts": {"listChanged": True}}
    if CAPS == "all-res":
        return {"tools": {}, "prompts": {"listChanged": True},
                "resources": {"listChanged": True}}
    if CAPS == "none":
        return {}
    if CAPS == "absent":
        return None
    if CAPS == "malformed":
        return "nope"
    return {"tools": {}}


def echo_tool(desc="echo the text argument"):
    return {
        "name": "echo",
        "description": desc,
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
        "annotations": {"readOnlyHint": True},
    }


def fail_tool():
    return {
        "name": "fail",
        "description": "always fails",
        "inputSchema": {"type": "object"},
        "annotations": {"destructiveHint": True},
    }


def bigschema_tool():
    # Deliberately larger than AGENTC_LIMIT_MCP_SCHEMA_BYTES (2048): the client
    # must substitute a valid fallback, never embed a half-cut schema.
    return {
        "name": "bigschema",
        "description": "oversized inputSchema",
        "inputSchema": {"type": "object", "pad": "x" * 3000},
    }


def badschema_tool():
    return {
        "name": "badschema",
        "description": "inputSchema is not an object",
        "inputSchema": [1, 2, 3],
    }


def trigger_tool():
    return {
        "name": "trigger",
        "description": "trigger a tools/list_changed re-sync",
        "inputSchema": {"type": "object"},
    }


def fresh_tool():
    return {
        "name": "fresh",
        "description": "newly added by the re-sync",
        "inputSchema": {"type": "object"},
    }


def greet_prompt(desc="greet the caller"):
    return {
        "name": "greet",
        "title": "Greeting",
        "description": desc,
        "arguments": [
            {"name": "name", "description": "who to greet", "required": True},
            {"name": "tone", "description": "tone of voice"},
        ],
    }


def summary_prompt():
    return {
        "name": "summary",
        "description": "summarize",
        "arguments": [{"name": "count", "description": "how many"}],
    }


def image_prompt():
    return {"name": "image", "description": "text plus a non-text block"}


def fail_prompt():
    return {"name": "fail", "description": "prompts/get fails"}


def big_prompt():
    return {"name": "big", "description": "over the text cap"}


def trigger_prompt():
    return {"name": "trigger", "description": "trigger a prompts/list_changed re-sync"}


def fresh_prompt():
    return {"name": "fresh", "description": "newly added by the prompt re-sync"}


def collide1_prompt():
    return {"name": "collide a", "description": "sanitizes to collide_a"}


def collide2_prompt():
    return {"name": "collide_a", "description": "collides with the other name"}


def res_a(desc="first"):
    return {"uri": "file:///a.txt", "name": "a", "title": "A file",
            "description": desc, "mimeType": "text/plain"}


def res_trigger():
    return {"uri": "file:///trigger", "name": "trigger",
            "description": "resources/list_changed trigger"}


def res_blob():
    return {"uri": "file:///blob.bin", "name": "blob",
            "mimeType": "application/octet-stream"}


def res_big_uri():
    # Over AGENTC_LIMIT_MCP_RESOURCE_URI: the client must skip it, never clip
    # the address into a different resource.
    return {"uri": "file:///" + "u" * 3000, "name": "big-uri"}


def tmpl_a():
    return {"uriTemplate": "file:///{path}", "name": "file",
            "description": "any file", "mimeType": "text/plain"}


def tmpl_b():
    return {"uriTemplate": "db://{table}/{id}", "name": "row",
            "description": "one row"}


def list_page(mid, cursor):
    if MUTATED:
        if cursor:
            result(mid, {"tools": [bigschema_tool(), badschema_tool()]})
        else:
            result(mid, {"tools": [echo_tool("echo v2 (mutated)"), fresh_tool()],
                         "nextCursor": "page2"})
    else:
        if cursor:
            result(mid, {"tools": [fail_tool(), badschema_tool(), trigger_tool()]})
        else:
            result(mid, {"tools": [echo_tool(), bigschema_tool()],
                         "nextCursor": "page2"})


def prompts_page(mid, cursor):
    if PROMPTS_MUTATED:
        if cursor:
            result(mid, {"prompts": [big_prompt(), trigger_prompt(), collide1_prompt(),
                                     collide2_prompt()]})
        else:
            result(mid, {"prompts": [greet_prompt("greet v2"), fresh_prompt(),
                                     image_prompt(), fail_prompt()],
                         "nextCursor": "pp2"})
    else:
        if cursor:
            result(mid, {"prompts": [big_prompt(), trigger_prompt(), collide1_prompt(),
                                     collide2_prompt()]})
        else:
            result(mid, {"prompts": [greet_prompt(), summary_prompt(), image_prompt(),
                                     fail_prompt()],
                         "nextCursor": "pp2"})


def prompts_get(mid, params):
    global PROMPTS_MUTATED
    name = params.get("name")
    args = params.get("arguments") or {}
    if name == "greet":
        blocks = [{"type": "text", "text": f"Hello {args.get('name', '')}"}]
        if args.get("tone"):
            blocks.append({"type": "text", "text": f"tone={args['tone']}"})
        result(mid, {"messages": [{"role": "assistant", "content": blocks}]})
    elif name == "summary":
        result(mid, {"messages": [{"role": "user", "content": {
            "type": "text", "text": f"summary {args.get('count', '')}"}}]})
    elif name == "image":
        result(mid, {"messages": [{"role": "assistant", "content": [
            {"type": "text", "text": "before"},
            {"type": "image", "data": "aGk=", "mimeType": "image/png"},
            {"type": "text", "text": "after"}]}]})
    elif name == "fail":
        reply({"jsonrpc": "2.0", "id": mid,
               "error": {"code": -32602, "message": "no such prompt"}})
    elif name == "big":
        result(mid, {"messages": [{"role": "assistant", "content": [
            {"type": "text", "text": "x" * 70000}]}]})
    elif name == "trigger":
        notify("notifications/prompts/list_changed")
        PROMPTS_MUTATED = True
        result(mid, {"messages": [{"role": "assistant", "content": [
            {"type": "text", "text": "mutated"}]}]})
    else:
        reply({"jsonrpc": "2.0", "id": mid,
               "error": {"code": -32602, "message": "unknown prompt"}})


def resources_page(mid, cursor):
    if RESOURCES_MUTATED:
        if cursor:
            result(mid, {"resources": [res_blob()]})
        else:
            result(mid, {"resources": [res_a("first v2"), res_trigger()],
                         "nextCursor": "rp2"})
    else:
        if cursor:
            result(mid, {"resources": [res_blob(), res_big_uri()]})
        else:
            result(mid, {"resources": [res_a(), res_trigger()],
                         "nextCursor": "rp2"})


def templates_page(mid, cursor):
    if cursor:
        result(mid, {"resourceTemplates": [tmpl_b()]})
    else:
        result(mid, {"resourceTemplates": [tmpl_a()], "nextCursor": "tp2"})


def resource_read(mid, params):
    global RESOURCES_MUTATED
    uri = params.get("uri")
    if uri == "file:///a.txt":
        result(mid, {"contents": [
            {"uri": uri, "mimeType": "text/plain", "text": "alpha"},
            {"uri": uri, "text": "beta"}]})
    elif uri == "file:///blob.bin":
        result(mid, {"contents": [{"uri": uri, "blob": "aGk=",
                                    "mimeType": "application/octet-stream"}]})
    elif uri == "file:///big.txt":
        result(mid, {"contents": [{"uri": uri, "text": "x" * 300000}]})
    elif uri == "file:///fail":
        reply({"jsonrpc": "2.0", "id": mid,
               "error": {"code": -32602, "message": "no such resource"}})
    elif uri == "file:///trigger":
        notify("notifications/resources/list_changed")
        RESOURCES_MUTATED = True
        result(mid, {"contents": [{"uri": uri, "text": "mutated"}]})
    else:
        reply({"jsonrpc": "2.0", "id": mid,
               "error": {"code": -32602, "message": "unknown resource"}})


def main():
    global MUTATED
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if LOG:
            with open(LOG, "a") as f:
                f.write(line + "\n")
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        method = msg.get("method")
        mid = msg.get("id")
        if method == "initialize":
            payload = {
                "protocolVersion": "2024-11-05",
                "serverInfo": {"name": "mcp-mock", "version": "1.0"},
            }
            caps = capabilities()
            if caps is not None:
                payload["capabilities"] = caps
            result(mid, payload)
            if PROMPT_NOTIFY:
                notify("notifications/prompts/list_changed")
            if RESOURCE_NOTIFY:
                notify("notifications/resources/list_changed")
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            list_page(mid, (msg.get("params") or {}).get("cursor"))
            arm_idle()
        elif method == "tools/call":
            params = msg.get("params") or {}
            name = params.get("name")
            args = params.get("arguments") or {}
            if name == "echo":
                text = str(args.get("text", ""))
                if GREETING:
                    text = f"{GREETING}:{text}"
                result(mid, {
                    "content": [{"type": "text", "text": text}],
                    "structuredContent": {"echo": text},
                })
            elif name == "fail":
                result(mid, {
                    "content": [{"type": "text", "text": "boom"}],
                    "isError": True,
                })
            elif name == "trigger":
                # Notification first, so the client processes it before the
                # matching response and marks a re-sync pending.
                notify("notifications/tools/list_changed")
                MUTATED = True
                result(mid, {"content": [{"type": "text", "text": "mutated"}]})
            else:
                reply({"jsonrpc": "2.0", "id": mid,
                       "error": {"code": -32602, "message": "unknown tool"}})
        elif method == "prompts/list":
            prompts_page(mid, (msg.get("params") or {}).get("cursor"))
        elif method == "prompts/get":
            prompts_get(mid, msg.get("params") or {})
        elif method == "resources/list":
            resources_page(mid, (msg.get("params") or {}).get("cursor"))
        elif method == "resources/templates/list":
            templates_page(mid, (msg.get("params") or {}).get("cursor"))
        elif method == "resources/read":
            resource_read(mid, msg.get("params") or {})
        elif mid is not None:
            reply({"jsonrpc": "2.0", "id": mid,
                   "error": {"code": -32601, "message": "method not found"}})

    # Stubborn mode: survive stdin EOF and SIGTERM until SIGKILL arrives.
    while IGNORE_SIGTERM:
        time.sleep(3600)


if __name__ == "__main__":
    main()
