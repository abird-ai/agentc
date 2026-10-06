#!/usr/bin/env python3
"""setup_e2e.py — first-run onboarding under a pty.

Drives `agentc setup --offline` through the provider menu and checks the files it
writes, then checks that a bare `agentc` offers onboarding only when nothing is
configured. No network: everything runs with --offline and an empty HOME.
"""
import json
import os
import pty
import select
import shutil
import stat
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FN = os.path.join(ROOT, "build", "agentc")

if not os.path.exists(FN):
    print("FAIL setup-e2e (build/agentc missing; run make release)")
    sys.exit(1)

fail = 0


def make_home(name):
    home = os.path.join(ROOT, "build", name)
    shutil.rmtree(home, ignore_errors=True)
    for sub in ("config", "data", "state"):
        os.makedirs(os.path.join(home, sub), exist_ok=True)
    return home, dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/config",
                      XDG_DATA_HOME=home + "/data", XDG_STATE_HOME=home + "/state")


def spawn(argv, env):
    master, slave = pty.openpty()
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(master)
        os.close(slave)
        os.execvpe(FN, argv, env)
    os.close(slave)
    return pid, master


def drain(master, seconds, until=None, cap=1 << 20):
    """Read for `seconds`, optionally stopping early once `until` appears."""
    out = b""
    end = time.time() + seconds
    while time.time() < end and len(out) < cap:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try:
                out += os.read(master, 65536)
            except OSError:
                break
        if until and until in out:
            break
    return out


def wait_exit(pid, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done == pid:
            return os.waitstatus_to_exitcode(status)
        time.sleep(0.05)
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    return None


def check(cond, label, extra=""):
    global fail
    if cond:
        print("ok   setup-e2e " + label)
    else:
        print("FAIL setup-e2e " + label + (": " + extra if extra else ""))
        fail = 1


# ---------------------------------------------------------- agentc setup (menu)
home, env = make_home("setup-e2e-home")
pid, master = spawn([FN, "setup", "--offline"], env)
out = drain(master, 5.0, until=b"Choice [1]:")
check(b"agentc first run" in out and b"Ollama" in out, "setup menu shown", repr(out[-200:]))
os.write(master, b"5\n")                                  # other provider
out = drain(master, 5.0, until=b"Provider [1]:")
check(b"openrouter" in out and b"gemini" in out, "preset list shown", repr(out[-200:]))
os.write(master, b"2\n")                                  # xai
out = drain(master, 5.0, until=b"API key:")
check(b"API key:" in out, "api key prompt", repr(out[-200:]))
os.write(master, b"sk-test-secret\n")
out = drain(master, 5.0, until=b"model id")
check(b"model id" in out, "model prompt with no cached models", repr(out[-200:]))
os.write(master, b"my-model\n")
out += drain(master, 2.0)
code = wait_exit(pid, 5.0)
check(code == 0, "setup exit code", str(code))
os.close(master)

setup_path = os.path.join(home, "config", "agentc", "setup.jsonc")
auth_path = os.path.join(home, "config", "agentc", "auth.jsonc")
try:
    setup = json.load(open(setup_path))
except Exception as e:                                    # noqa: BLE001
    setup = None
    check(False, "setup.jsonc written", str(e))
if setup is not None:
    check(setup.get("default_provider") == "xai" and setup.get("default_model") == "my-model",
          "setup.jsonc contents", json.dumps(setup))
try:
    auth = json.load(open(auth_path))
except Exception as e:                                    # noqa: BLE001
    auth = None
    check(False, "auth.jsonc written", str(e))
if auth is not None:
    check(auth.get("xai", {}).get("api_key") == "sk-test-secret", "auth.jsonc contents",
          json.dumps(auth))
    mode = stat.S_IMODE(os.stat(auth_path).st_mode)
    check(mode == 0o600, "auth.jsonc mode 0600", oct(mode))

# ------------------------------------------------- configured: no onboarding
pid, master = spawn([FN], env)                            # bare agentc, setup.jsonc present
out = drain(master, 2.5)
check(b"agentc first run" not in out, "no onboarding when configured", repr(out[:200]))
os.write(master, b"\x03")                                # clear the editor
time.sleep(0.2)
os.write(master, b"/quit\n")
code = wait_exit(pid, 5.0)
check(code == 0, "configured run exits cleanly", str(code))
os.close(master)

# ------------------------------------------------------ bare agentc: onboarding
home2, env2 = make_home("setup-e2e-empty")
pid, master = spawn([FN], env2)
out = drain(master, 5.0, until=b"Choice [1]:")
check(b"agentc first run" in out, "onboarding offered on a bare run", repr(out[:200]))
os.write(master, b"q\n")
out += drain(master, 1.0)
code = wait_exit(pid, 5.0)
check(code == 1 and b"setup cancelled" in out, "quitting onboarding exits 1", str(code))
os.close(master)

# --------------------------------------- setup.jsonc is lower than config
# a config.jsonc default must win over setup.jsonc
os.makedirs(os.path.join(home2, "config", "agentc"), exist_ok=True)
with open(os.path.join(home2, "config", "agentc", "setup.jsonc"), "w") as f:
    f.write('{"provider": "ollama", "model": "ignored"}\n')
with open(os.path.join(home2, "config", "agentc", "config.jsonc"), "w") as f:
    f.write('{\n  // explicit user choice\n  "provider": "openrouter",\n'
            '  "model": "chosen",\n  "api_key": "sk-x",\n}\n')
r = subprocess.run([FN, "--list-sessions"], env=env2, capture_output=True, timeout=20)
check(r.returncode == 0, "config.jsonc overrides setup.jsonc (load ok)",
      r.stderr.decode(errors="replace"))

if fail == 0:
    print("ok   setup-e2e (menu, auth/setup files, onboarding trigger)")
sys.exit(fail)
