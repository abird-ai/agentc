#!/usr/bin/env python3
"""rpc_check.py — drive the real `agentc --mode rpc` over a pipe against the
mock provider and assert the RPC v1 record layer end to end: framing,
dual-discriminator resolution, command echo, machine codes, id forms,
acceptance-first + agent_settled, and the dropped ui_response.

Run after a release build (tests/e2e.sh calls it). Prints ok/FAIL lines and
exits nonzero on failure.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FN = os.environ.get("AGENTC_BIN", os.path.join(ROOT, "build", "agentc"))
MOCK = os.path.join(ROOT, "tests", "mock_provider.py")

if not os.path.exists(FN):
    print("FAIL rpc-check (build/agentc missing; run make release)")
    sys.exit(1)

home = os.path.join(ROOT, "build", "rpc-check-home")
for sub in ("config", "data", "state"):
    os.makedirs(os.path.join(home, sub), exist_ok=True)
env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/config",
           XDG_DATA_HOME=home + "/data", XDG_STATE_HOME=home + "/state")

srv = subprocess.Popen([sys.executable, MOCK], stdout=subprocess.PIPE, env=env)
port = srv.stdout.readline().decode().strip()
url = "http://127.0.0.1:" + port

commands = [
    {"type": "hello", "id": 1},
    {"id": "state-1", "command": "get_state"},
    {"id": 3, "command": "get_commands"},
    {"id": 4, "command": "set_thinking_level", "thinking_level_name": "high"},
    {"id": 5, "command": "set_thinking_level", "thinking_level": 2},
    {"id": 6, "command": "get_available_models"},
    "not json",
    {"id": 8},
    {"id": 9, "command": "bogus"},
    {"type": "ui_response", "id": 99, "result": {}},
    {"id": 10, "command": "prompt", "text": "read the allocator"},
]
payload = "\n".join(json.dumps(c) if not isinstance(c, str) else c for c in commands) + "\n"

proc = subprocess.Popen(
    [FN, "--mode", "rpc", "--api-key", "test", "--provider", "anthropic",
     "--model", "claude-sonnet-4-5", "--base-url", url],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    env=env, text=True)
try:
    out, err = proc.communicate(payload, timeout=90)
except subprocess.TimeoutExpired:
    proc.kill()
    srv.terminate()
    print("FAIL rpc-check (timeout)")
    print(err[-2000:] if err else "")
    sys.exit(1)

objs = []
for line in out.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        v = json.loads(line)
    except ValueError:
        continue
    if isinstance(v, dict):
        objs.append(v)

fail = 0


def ok(name, cond, detail=""):
    global fail
    if cond:
        print("ok   rpc-check " + name)
    else:
        print("FAIL rpc-check " + name + (": " + detail if detail else ""))
        fail = 1


def responses(command=None):
    return [o for o in objs if o.get("type") == "response"
            and (command is None or o.get("command") == command)]


by_id = {}
for o in responses():
    by_id[str(o.get("id"))] = o

hello = by_id.get("1")
ok("hello", hello is not None and hello.get("success") is True
   and hello.get("code") == "ok" and hello.get("data", {}).get("protocol") == "agentc-rpc"
   and hello.get("data", {}).get("version") == 1
   and "get_commands" in hello.get("data", {}).get("commands", []))
ok("hello.capabilities", "acceptance" in hello.get("data", {}).get("capabilities", [])
   if hello else False)

st = next((o for o in responses("get_state") if o.get("id") == "state-1"), None)
d = st.get("data", {}) if st else {}
ok("get_state.new_fields", st is not None and st.get("success") is True
   and d.get("protocol") == "agentc-rpc" and "thinking_level_name" in d
   and "session_file" in d and "is_streaming" in d and "pending_commands" in d
   and "auto_compaction" in d)
ok("get_state.thinking_name", d.get("thinking_level_name") in ("off", "low", "medium", "high"))

cmds = next((o for o in responses("get_commands")), None)
ok("get_commands", cmds is not None and cmds.get("success") is True
   and isinstance(cmds.get("data", {}).get("commands"), list))

th = next((o for o in responses("set_thinking_level") if o.get("id") == 4), None)
ok("thinking.name", th is not None and th.get("success") is True
   and th.get("data", {}).get("thinking_level_name") == "high")
bad = next((o for o in responses("set_thinking_level") if o.get("id") == 5), None)
ok("thinking.reject_two", bad is not None and bad.get("success") is False
   and bad.get("code") == "invalid_level")

models = next((o for o in responses("get_available_models")), None)
ok("models.total", models is not None and isinstance(models.get("data", {}).get("total"), int)
   and models.get("data", {}).get("total") >= 1)

inv = next((o for o in responses() if o.get("code") == "invalid_json"), None)
ok("invalid_json", inv is not None and inv.get("command") is None and inv.get("id") is None)
miss = next((o for o in responses() if o.get("code") == "missing_command"), None)
ok("missing_command", miss is not None and miss.get("id") == 8)
unk = next((o for o in responses("bogus")), None)
ok("unknown_command", unk is not None and unk.get("success") is False)
ok("ui_response.dropped", not any(o.get("id") == 99 for o in responses()))

accepted = next((o for o in responses("prompt") if o.get("id") == 10), None)
ok("prompt.started", accepted is not None and accepted.get("success") is True
   and accepted.get("data", {}).get("disposition") == "started")
settled = [o for o in objs if o.get("type") == "agent_settled"]
ok("prompt.settled", len(settled) == 1 and settled[0].get("disposition") == "completed")
ok("prompt.no_second_response",
   len([o for o in responses("prompt") if o.get("id") == 10]) == 1)

srv.terminate()
sys.exit(fail)
