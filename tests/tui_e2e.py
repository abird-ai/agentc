#!/usr/bin/env python3
"""tui_e2e.py — drive the real TUI under a pty against the mock provider and
check the full loop plus session persistence.

Run after a release build (tests/e2e.sh calls it). Prints ok/FAIL lines and
exits nonzero on failure.
"""
import glob
import json
import os
import pty
import re
import select
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FN = os.environ.get("AGENTC_BIN", os.path.join(ROOT, "build", "agentc"))
MOCK = os.path.join(ROOT, "tests", "mock_provider.py")

if not os.path.exists(FN):
    print("FAIL tui-e2e (build/agentc missing; run make release)")
    sys.exit(1)

home = os.path.join(ROOT, "build", "tui-e2e-home")
for sub in ("config", "data", "state"):
    os.makedirs(os.path.join(home, sub), exist_ok=True)
env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/config",
           XDG_DATA_HOME=home + "/data", XDG_STATE_HOME=home + "/state",
           TERM="xterm-256color")

srv = subprocess.Popen([sys.executable, MOCK], stdout=subprocess.PIPE, env=env)
port = srv.stdout.readline().decode().strip()

master, slave = pty.openpty()
pid = os.fork()
if pid == 0:
    os.setsid()
    os.dup2(slave, 0)
    os.dup2(slave, 1)
    os.dup2(slave, 2)
    os.close(master)
    os.close(slave)
    os.execvpe(FN, [FN, "--api-key", "test",
                    "--provider", "anthropic", "--model", "claude-sonnet-4-5",
                    "--base-url", "http://127.0.0.1:" + port], env)
os.close(slave)


def drain(seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try:
                out += os.read(master, 65536)
            except OSError:
                break
    return out


time.sleep(0.4)
boot = drain(0.3)
os.write(master, b"read the allocator\n")
screen = drain(2.5)
os.write(master, b"/quit\n")
time.sleep(0.5)
drain(0.3)
_, status, _ = os.wait4(pid, 0)

fail = 0
if os.waitstatus_to_exitcode(status) != 0:
    print("FAIL tui-e2e exit code")
    fail = 1
if b"done" not in screen:
    print("FAIL tui-e2e streamed answer")
    fail = 1

files = glob.glob(os.path.join(home, "data", "abird", "agentc", "sessions", "**", "*.jsonl"),
                  recursive=True)
if not files:
    print("FAIL tui-e2e session file")
    fail = 1
else:
    lines = open(files[0]).read().splitlines()
    roles = [json.loads(l).get("role") for l in lines[1:]
             if json.loads(l).get("type") == "message"]
    if roles != ["user", "assistant", "tool", "assistant"]:
        print("FAIL tui-e2e session roles:", roles)
        fail = 1

# ------------------------------------------------- redraw stability (keys)
# Replay the raw stream a real terminal would receive: after several keypresses
# the footer must still occupy exactly one row (the old bug printed a new footer
# line on every keypress and filled the screen).
sys.path.insert(0, os.path.join(ROOT, "tests"))
from pty_screen import Screen  # noqa: E402

# The empty-transcript hint must be on the startup frame and gone once the
# first message has landed in the transcript (the pty is 80x24 by fallback).
HINT = "Ready. Press Ctrl-C once to clear, twice to exit"
screen1 = Screen(80, 24).feed(boot)
if HINT not in screen1.text():
    print("FAIL tui-e2e empty-state hint missing at startup")
    print(screen1.text())
    fail = 1
screen1 = Screen(80, 24).feed(boot + screen)
if HINT in screen1.text():
    print("FAIL tui-e2e empty-state hint survived the first message")
    print(screen1.text())
    fail = 1

master3, slave3 = pty.openpty()
rows, cols = 24, 100
import fcntl
import struct
import termios as _t
fcntl.ioctl(slave3, _t.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
pid3 = os.fork()
if pid3 == 0:
    os.setsid()
    os.dup2(slave3, 0)
    os.dup2(slave3, 1)
    os.dup2(slave3, 2)
    os.close(master3)
    os.close(slave3)
    os.execvpe(FN, [FN, "--api-key", "t", "--offline",
                    "--no-session"], env)
os.close(slave3)
time.sleep(0.6)
out3 = b""


def pump3(seconds):
    global out3
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master3], [], [], 0.05)
        if r:
            try:
                out3 += os.read(master3, 65536)
            except OSError:
                return


def wait_for(needle, seconds):
    """Pump until `needle` shows up on the wire (fixed sleeps race the TUI)."""
    end = time.time() + seconds
    while time.time() < end:
        if needle in out3:
            return True
        pump3(0.05)
    return needle in out3


wait_for(b"tok:0/0", 5.0)                 # first frame: the TUI is up

# The placeholder is on the first frame, exactly once, inside the composer and
# with no background band; typing replaces it (checked below).
# a background is either an explicit 48;… colour or a 16-colour 40-47/100-107
bg_re = re.compile(rb"\x1b\[(?:48;|4[0-7]m|10[0-7]m)")
screen0 = Screen(cols, rows).feed(out3)
if HINT not in screen0.text():
    print("FAIL tui-e2e empty-state hint missing on the first frame")
    print(screen0.text())
    fail = 1
if screen0.count_rows_containing(HINT) != 1:
    print("FAIL tui-e2e empty-state hint must occupy exactly one row")
    print(screen0.text())
    fail = 1
# the placeholder row is the first fullscreen diff row that carries the hint;
# it must be emitted with the reverse-video caret cell on the first glyph and a
# dim muted tail, and with no background band (a background is either an
# explicit 48;… colour or a 16-colour 40-47/100-107). The fullscreen diff emits
# each changed row as ESC[<row>;1H followed by that row's bytes.
def frame_rows(data):
    rows = {}
    hits = list(re.finditer(rb"\x1b\[(\d+);1H", data))
    for i, m in enumerate(hits):
        end = hits[i + 1].start() if i + 1 < len(hits) else len(data)
        # the frame ends by parking the cursor with ESC[<row>;<col>H right
        # before ESC[?25h; that is not a row paint, so do not shadow the row's
        # last real diff.
        if data[m.end():m.end() + 6] == b"\x1b[?25h":
            continue
        rows[int(m.group(1))] = data[m.start():end]
    return rows


rows0 = frame_rows(out3)


def row_hits(screen, needle):
    return [i for i, row in enumerate(screen.grid) if needle in "".join(row)]


hint_rows = row_hits(screen0, HINT)
hint_bytes = rows0.get(hint_rows[0] + 1, b"") if hint_rows else b""
if not hint_rows or b"eady. Press Ctrl-C" not in hint_bytes:
    print("FAIL tui-e2e empty-state hint missing from the raw stream")

    fail = 1
elif not (b"\x1b[7m" in hint_bytes and b"\x1b[2m" in hint_bytes):
    print("FAIL tui-e2e placeholder not dim under the reversed caret")
    fail = 1
elif bg_re.search(hint_bytes):
    print("FAIL tui-e2e empty-state hint painted a background")
    fail = 1

for _ in range(4):
    os.write(master3, b"a")
wait_for(b"aaaa", 2.0)
pump3(0.2)                                # let the frame finish landing

# what the user sees right now, before quitting: the placeholder was replaced
# by the first keystroke (it is composer decoration, not transcript content)
screen = Screen(cols, rows).feed(out3)
if HINT in screen.text():
    print("FAIL tui-e2e placeholder survived into the typed composer")
    print(screen.text())
    fail = 1
footer_rows = screen.count_rows_containing("tok:0/0")
if footer_rows != 1:
    print("FAIL tui-e2e redraw drift: footer on", footer_rows, "rows")
    print(screen.text())
    fail = 1
if "aaaa" not in screen.text():
    print("FAIL tui-e2e editor text lost after keypresses")
    print(screen.text())
    fail = 1
# the composer is two rules one column short of the width (U+2500, or '-' on a
# dumb terminal; N25/ADR-22 reserves the last column so a width reflow cannot
# re-wrap the rule) bracketing the input; the old highlighted "> " marker must
# be gone
rules = {"\u2500" * (cols - 1), "-" * (cols - 1)}
text_row = None
rule_rows = []
for idx, row in enumerate(screen.grid):
    line = "".join(row).rstrip()
    if line in rules:
        rule_rows.append(idx)
    if "aaaa" in line:
        text_row = idx
if (len(rule_rows) != 2 or text_row is None
        or rule_rows[0] != text_row - 1 or rule_rows[1] != text_row + 1):
    print("FAIL tui-e2e composer rules around the input")
    print(screen.text())
    fail = 1
if "> aaaa" in screen.text():
    print("FAIL tui-e2e highlighted prompt marker still present")
    print(screen.text())
    fail = 1

# The rules must sit on the terminal's own background while the status line
# still paints the theme band: inspect the SGR of the last fullscreen frames
# for those rows.
rows_now = frame_rows(out3)
rule_segs = [rows_now[r + 1] for r in rule_rows if r + 1 in rows_now]
if not rule_segs:
    print("FAIL tui-e2e no composer rule rows in the raw stream")
    fail = 1
elif any(bg_re.search(s) for s in rule_segs):
    print("FAIL tui-e2e composer rule row painted a background")
    fail = 1
status_rows_now = row_hits(screen, "ready")
status_segs = [rows_now[r + 1] for r in status_rows_now if r + 1 in rows_now]
if not any(bg_re.search(s) for s in status_segs):
    print("FAIL tui-e2e status line lost its background band")
    fail = 1

# /model opens the picker; filtering and Enter switches the model
os.write(master3, b"\x03")          # clear the editor
pump3(0.2)
os.write(master3, b"/model\n")
if not wait_for(b"gpt-4.1", 3.0):
    print("FAIL tui-e2e /model picker did not open")
    fail = 1
os.write(master3, b"4.1\n")        # narrow to gpt-4.1 and select it
if not wait_for(b"model: gpt-4.1", 3.0):
    print("FAIL tui-e2e /model picker did not switch")
    fail = 1
# /model <id> still switches by exact id
os.write(master3, b"/model llama3.2\n")
if not wait_for(b"model: llama3.2", 3.0):
    print("FAIL tui-e2e /model <id> did not switch")
    fail = 1
os.write(master3, b"/help\n")
if not wait_for(b"/model [id|alias], /theme [dark|light]", 3.0):
    print("FAIL tui-e2e /help did not list /model")
    fail = 1
screen2 = Screen(cols, rows).feed(out3)
if "model: gpt-4.1" not in screen2.text() and "model: llama3.2" not in screen2.text():
    print("FAIL tui-e2e /model notice not on screen")
    print(screen2.text())
    fail = 1

# /compact reports (an error without a transport is still a report), and /new
# starts a fresh session through the app callback.
os.write(master3, b"/compact\n")
if not wait_for(b"compact:", 3.0):
    print("FAIL tui-e2e /compact did not report")
    fail = 1
os.write(master3, b"/new\n")
if not wait_for(b"new: started a new session", 3.0):
    print("FAIL tui-e2e /new did not start a session")
    fail = 1

os.write(master3, b"\x03")          # Ctrl+C clears the editor
pump3(0.2)
os.write(master3, b"/quit\n")
pump3(0.6)

# Empty-exit trace: this session never submitted a prompt (/new cleared the
# view), so the scrollback dump must be the banner alone -- no dangling "..."
# and no second copy of the header.
leave3 = out3.rfind(b"\x1b[?1049l")
dump3 = out3[leave3:] if leave3 >= 0 else b""
if b"..." in dump3:
    print("FAIL tui-e2e empty exit dump printed the '...' separator")
    fail = 1
if dump3.count(b"agentc 0.6.0") > 1:
    print("FAIL tui-e2e empty exit dump repeated the banner")
    fail = 1

# On the normal quit path the terminal must end with the cursor restored: a
# show after the last hide, so a killed or quit session never leaves it hidden.
if b"\x1b[?25h" not in out3 or out3.rfind(b"\x1b[?25h") < out3.rfind(b"\x1b[?25l"):
    print("FAIL tui-e2e cursor not restored on quit")
    fail = 1

# watchdog: a wedged TUI must not hang the suite
deadline = time.time() + 5
status3 = None
while time.time() < deadline:
    done, st = os.waitpid(pid3, os.WNOHANG)
    if done == pid3:
        status3 = st
        break
    time.sleep(0.1)
if status3 is None:
    os.kill(pid3, signal.SIGKILL)
    _, status3 = os.waitpid(pid3, 0)
    print("FAIL tui-e2e redraw phase: TUI did not quit")
    fail = 1

os.close(master3)

# -------------------------------------------- slash menu placement (pty)
# The menu is chrome: above the composer in fullscreen, below it inline, and
# gone once dismissed — never left on screen as a committed row. Fresh ptys
# keep the geometry unambiguous.
def menu_spawn(mcols, mrows):
    mmaster, mslave = pty.openpty()
    fcntl.ioctl(mslave, _t.TIOCSWINSZ, struct.pack("HHHH", mrows, mcols, 0, 0))
    mpid = os.fork()
    if mpid == 0:
        os.setsid()
        os.dup2(mslave, 0)
        os.dup2(mslave, 1)
        os.dup2(mslave, 2)
        os.close(mmaster)
        os.close(mslave)
        os.execvpe(FN, [FN, "--api-key", "t", "--offline", "--no-session"], env)
    os.close(mslave)
    return mmaster, mpid


def menu_collect(mmaster, seconds):
    data = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([mmaster], [], [], 0.05)
        if r:
            try:
                data += os.read(mmaster, 65536)
            except OSError:
                break
    return data


def menu_kill(mmaster, mpid):
    os.kill(mpid, signal.SIGTERM)
    time.sleep(0.2)
    try:
        os.close(mmaster)
    except OSError:
        pass
    try:
        os.waitpid(mpid, 0)
    except ChildProcessError:
        pass

# fullscreen: the menu sits above the composer's top rule
master_f, pid_f = menu_spawn(80, 24)
text = menu_collect(master_f, 0.7)
os.write(master_f, b"/")
text += menu_collect(master_f, 0.5)
screen_f = Screen(80, 24).feed(text)
rules_f = row_hits(screen_f, "\u2500" * 79)
menu_f = row_hits(screen_f, "/model")
if not menu_f or not rules_f or menu_f[0] > rules_f[0]:
    print("FAIL tui-e2e fullscreen menu placement", rules_f, menu_f)
    print(screen_f.text())
    fail = 1
menu_kill(master_f, pid_f)

# ---------------------------------------- /model long alias + empty filter
# A 60-byte config-declared alias is longer than the picker's old 47-byte label
# buffer. Selecting it must set the full id (the notice names the concrete
# route); filtering on a suffix past byte 47 must still find it. A filter that
# matches nothing must keep the pane visible with a placeholder, and Esc must
# still hand the keyboard back to the editor.
alias = "e2e-alias-" + "0123456789" * 4 + "UNIQUE-END"
assert len(alias) == 60, len(alias)
ahome = os.path.join(ROOT, "build", "tui-e2e-alias-home")
subprocess.run(["rm", "-rf", ahome], check=False)
for _sub in ("config", "data", "state"):
    os.makedirs(os.path.join(ahome, _sub), exist_ok=True)
cfg_dir = os.path.join(ahome, "config", "abird", "agentc")
os.makedirs(cfg_dir, exist_ok=True)
with open(os.path.join(cfg_dir, "config.jsonc"), "w") as _f:
    _f.write('{ "models": { "%s": { "provider": "openai", "model": "gpt-4.1" } } }\n'
             % alias)
aenv = dict(env, HOME=ahome, XDG_CONFIG_HOME=ahome + "/config",
            XDG_DATA_HOME=ahome + "/data", XDG_STATE_HOME=ahome + "/state")

master_a, slave_a = pty.openpty()
fcntl.ioctl(slave_a, _t.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
pid_a = os.fork()
if pid_a == 0:
    os.setsid()
    os.dup2(slave_a, 0)
    os.dup2(slave_a, 1)
    os.dup2(slave_a, 2)
    os.close(master_a)
    os.close(slave_a)
    os.execvpe(FN, [FN, "--api-key", "t", "--offline", "--no-session"], aenv)
os.close(slave_a)
out_a = b""


def pump_a(seconds):
    global out_a
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master_a], [], [], 0.05)
        if r:
            try:
                out_a += os.read(master_a, 65536)
            except OSError:
                return


def wait_a(needle, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if needle in out_a:
            return True
        pump_a(0.05)
    return needle in out_a


time.sleep(0.6)
pump_a(0.4)
os.write(master_a, b"/model\n")
if not wait_a(alias.encode(), 3.0):
    print("FAIL tui-e2e long alias: picker did not list the full id")
    print(Screen(100, 24).feed(out_a).text())
    fail = 1
os.write(master_a, b"UNIQUE-END")
pump_a(0.3)
os.write(master_a, b"\r")
if not wait_a((alias + " (alias ->").encode(), 3.0):
    print("FAIL tui-e2e long alias: picker set a truncated id")
    fail = 1

# A no-hit filter keeps the pane up; Esc closes it and the editor takes keys.
os.write(master_a, b"/model\n")
pump_a(0.3)
os.write(master_a, b"zzzznomatch")
if not wait_a(b"(no matches)", 3.0):
    print("FAIL tui-e2e empty filter: picker pane vanished")
    fail = 1
os.write(master_a, b"\x1b")
pump_a(0.5)
os.write(master_a, b"ZZMARK")
pump_a(0.4)
if "ZZMARK" not in Screen(100, 24).feed(out_a).text():
    print("FAIL tui-e2e empty filter: Esc did not restore the editor")
    print(Screen(100, 24).feed(out_a).text())
    fail = 1
os.write(master_a, b"\x03")
pump_a(0.2)
os.write(master_a, b"/quit\n")
pump_a(0.6)
deadline_a = time.time() + 5
status_a = None
while time.time() < deadline_a:
    done, st_a = os.waitpid(pid_a, os.WNOHANG)
    if done == pid_a:
        status_a = st_a
        break
    time.sleep(0.1)
if status_a is None:
    os.kill(pid_a, signal.SIGKILL)
    _, status_a = os.waitpid(pid_a, 0)
    print("FAIL tui-e2e alias phase: TUI did not quit")
    fail = 1
elif os.waitstatus_to_exitcode(status_a) != 0:
    print("FAIL tui-e2e alias phase exit code")
    fail = 1
os.close(master_a)

# ---------------------------------------------------------------- signals
# Killing the TUI must restore the terminal: cursor shown, paste disabled and
# the tty back in canonical mode.
import termios

master2, slave2 = pty.openpty()
pid2 = os.fork()
if pid2 == 0:
    os.setsid()
    os.dup2(slave2, 0)
    os.dup2(slave2, 1)
    os.dup2(slave2, 2)
    os.close(master2)
    os.close(slave2)
    os.execvpe(FN, [FN, "--api-key", "t", "--offline",
                    "--no-session"], env)
os.close(slave2)
time.sleep(0.6)
out2 = b""
r, _, _ = select.select([master2], [], [], 0.3)
if r:
    out2 += os.read(master2, 65536)
os.kill(pid2, signal.SIGTERM)
time.sleep(0.6)
r, _, _ = select.select([master2], [], [], 0.3)
if r:
    try:
        out2 += os.read(master2, 65536)
    except OSError:
        pass
_, status2, _ = os.wait4(pid2, 0)
code = os.waitstatus_to_exitcode(status2)
if code != 128 + signal.SIGTERM:
    print("FAIL tui-e2e signal exit:", code)
    fail = 1
if b"\x1b[?25h" not in out2 or b"\x1b[?2004l" not in out2:
    print("FAIL tui-e2e signal terminal restore sequences")
    fail = 1
if out2.rfind(b"\x1b[?25h") < out2.rfind(b"\x1b[?25l"):
    print("FAIL tui-e2e cursor hidden after SIGTERM restore")
    fail = 1
try:
    lflag = termios.tcgetattr(master2)[3]
    if not (lflag & termios.ICANON):
        print("FAIL tui-e2e termios not restored (still raw)")
        fail = 1
except termios.error as e:
    print("FAIL tui-e2e termios query:", e)
    fail = 1
os.close(master2)

# ------------------------------------------------ mid-stream steer/interrupt
# The mock "PACE" mode stalls between SSE events. Escape during the stall must
# abort the in-flight body read through the HTTP cancel predicate, not wait for
# the request deadline or the next chunk.
master5, slave5 = pty.openpty()
fcntl.ioctl(slave5, _t.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
pid5 = os.fork()
if pid5 == 0:
    os.setsid()
    os.dup2(slave5, 0)
    os.dup2(slave5, 1)
    os.dup2(slave5, 2)
    os.close(master5)
    os.close(slave5)
    os.execvpe(FN, [FN, "--api-key", "test",
                    "--provider", "anthropic", "--model", "claude-sonnet-4-5",
                    "--base-url", "http://127.0.0.1:" + port], env)
os.close(slave5)
out5 = b""


def pump5(seconds):
    global out5
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master5], [], [], 0.05)
        if r:
            try:
                out5 += os.read(master5, 65536)
            except OSError:
                return


def wait5(needle, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if needle in out5:
            return True
        pump5(0.05)
    return needle in out5


time.sleep(0.6)
pump5(0.4)
os.write(master5, b"PACE please\n")
if not wait5(b"done", 6.0):
    print("FAIL tui-e2e steer: paced stream never delivered text")
    fail = 1
os.write(master5, b"\x1b")          # Esc aborts the in-flight body read
if not wait5(b"aborted", 6.0):
    print("FAIL tui-e2e steer: Esc did not abort the in-flight stream")
    fail = 1
os.write(master5, b"/quit\n")
pump5(0.6)
deadline5 = time.time() + 5
status5 = None
while time.time() < deadline5:
    done, st5 = os.waitpid(pid5, os.WNOHANG)
    if done == pid5:
        status5 = st5
        break
    time.sleep(0.1)
if status5 is None:
    os.kill(pid5, signal.SIGKILL)
    _, status5 = os.waitpid(pid5, 0)
    print("FAIL tui-e2e steer: TUI did not quit after the abort")
    fail = 1
elif os.waitstatus_to_exitcode(status5) != 0:
    print("FAIL tui-e2e steer exit code")
    fail = 1
os.close(master5)

# --------------------------------------------------- async approval overlay
# --approval ask plus a write outside the workspace parks the core `approval`
# hook; the TUI must render the overlay and answer with
# agentc_approval_submit. 'n' denies (fail-closed) and the turn finishes.
master6, slave6 = pty.openpty()
fcntl.ioctl(slave6, _t.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
pid6 = os.fork()
if pid6 == 0:
    os.setsid()
    os.dup2(slave6, 0)
    os.dup2(slave6, 1)
    os.dup2(slave6, 2)
    os.close(master6)
    os.close(slave6)
    os.execvpe(FN, [FN, "--api-key", "test", "--approval", "ask",
                    "--provider", "anthropic", "--model", "claude-sonnet-4-5",
                    "--base-url", "http://127.0.0.1:" + port], env)
os.close(slave6)
out6 = b""


def pump6(seconds):
    global out6
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master6], [], [], 0.05)
        if r:
            try:
                out6 += os.read(master6, 65536)
            except OSError:
                return


def wait6(needle, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if needle in out6:
            return True
        pump6(0.05)
    return needle in out6


time.sleep(0.6)
pump6(0.4)
os.write(master6, b"APPROVE please\n")
if not wait6(b"Approve write", 8.0):
    print("FAIL tui-e2e approval: overlay not rendered")
    fail = 1
if not wait6(b"agentc-e2e-approval.txt", 2.0):
    print("FAIL tui-e2e approval: tool input not shown")
    fail = 1
if b"scope:" not in out6 or b"[p]roject" not in out6:
    print("FAIL tui-e2e approval: scope picker not shown")
    fail = 1
os.write(master6, b"s")          # pick the step scope
os.write(master6, b"r")          # toggle the remembered-decision choice
pump6(0.3)
os.write(master6, b"n")          # deny: answered through agentc_approval_submit
if not wait6(b"done", 8.0):
    print("FAIL tui-e2e approval: denied turn did not finish")
    fail = 1
os.write(master6, b"/quit\n")
pump6(0.6)
deadline6 = time.time() + 5
status6 = None
while time.time() < deadline6:
    done, st6 = os.waitpid(pid6, os.WNOHANG)
    if done == pid6:
        status6 = st6
        break
    time.sleep(0.1)
if status6 is None:
    os.kill(pid6, signal.SIGKILL)
    _, status6 = os.waitpid(pid6, 0)
    print("FAIL tui-e2e approval: TUI did not quit after the denial")
    fail = 1
elif os.waitstatus_to_exitcode(status6) != 0:
    print("FAIL tui-e2e approval exit code")
    fail = 1
os.close(master6)

# ------------------------------------------------- settings editor (ext build)
# The stock release links no setting-declaring extension, so /settings has
# nothing to edit and this scenario skips. `make settings-e2e` builds
# build/agentc-exts from the same release objects plus the manifest's static
# extensions (realtime-voice declares enabled/mode/voice). Drive the real
# single-value editor end to end: open the picker, filter to the
# realtime-voice STRING row, edit it, save, and prove it persisted.
SETTINGS_FN = os.environ.get("AGENTC_SETTINGS_BIN",
                             os.path.join(ROOT, "build", "agentc-exts"))
if not os.path.exists(SETTINGS_FN):
    print("skip tui-e2e settings editor (%s not built; run make settings-e2e)"
          % os.path.relpath(SETTINGS_FN, ROOT))
else:
    shome = os.path.join(ROOT, "build", "tui-e2e-settings-home")
    subprocess.run(["rm", "-rf", shome], check=False)
    for sub in ("config", "data", "state"):
        os.makedirs(os.path.join(shome, sub), exist_ok=True)
    senv = dict(env, HOME=shome,
                XDG_CONFIG_HOME=shome + "/config",
                XDG_DATA_HOME=shome + "/data",
                XDG_STATE_HOME=shome + "/state")
    master7, slave7 = pty.openpty()
    fcntl.ioctl(slave7, _t.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
    pid7 = os.fork()
    if pid7 == 0:
        os.setsid()
        os.dup2(slave7, 0)
        os.dup2(slave7, 1)
        os.dup2(slave7, 2)
        os.close(master7)
        os.close(slave7)
        os.execvpe(SETTINGS_FN, [SETTINGS_FN, "--api-key", "t", "--offline",
                                 "--no-session"], senv)
    os.close(slave7)
    out7 = b""

    def pump7(seconds):
        global out7
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([master7], [], [], 0.05)
            if r:
                try:
                    out7 += os.read(master7, 65536)
                except OSError:
                    return

    def wait7(needle, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if needle in out7:
                return True
            pump7(0.05)
        return needle in out7

    time.sleep(0.6)
    pump7(0.3)
    os.write(master7, b"/settings\r")
    if not wait7(b"realtime-voice.enabled", 4.0):
        print("FAIL tui-e2e settings: picker did not list the declared settings")
        fail = 1
    else:
        os.write(master7, b".voice")   # one row: realtime-voice.voice
        pump7(0.3)
        os.write(master7, b"\r")       # open the single-value editor
        if not wait7(b"edit realtime-voice.voice", 3.0):
            print("FAIL tui-e2e settings: editor did not open")
            fail = 1
        else:
            os.write(master7, b"\x7f" * len(b"alloy"))   # clear the default
            os.write(master7, b"e2e-voice")
            pump7(0.2)
            os.write(master7, b"\r")                     # save (fail-closed store)
            if not wait7(b"settings: realtime-voice.voice updated", 3.0):
                print("FAIL tui-e2e settings: save notice missing")
                fail = 1
            cfg = os.path.join(shome, "config", "abird", "agentc", "config.jsonc")
            persisted = False
            if os.path.exists(cfg):
                blob = open(cfg, "rb").read()
                persisted = (b'"realtime-voice"' in blob and b'"voice"' in blob and
                             b'"e2e-voice"' in blob)
            if not persisted:
                print("FAIL tui-e2e settings: value not persisted to config.jsonc")
                fail = 1
            else:
                print("ok   tui-e2e settings editor (open, edit, save, persisted)")
    os.write(master7, b"/quit\r")
    pump7(0.5)
    deadline7 = time.time() + 5
    done7 = False
    while time.time() < deadline7:
        d7, st7 = os.waitpid(pid7, os.WNOHANG)
        if d7 == pid7:
            done7 = True
            if os.waitstatus_to_exitcode(st7) != 0:
                print("FAIL tui-e2e settings exit code")
                fail = 1
            break
        time.sleep(0.1)
    if not done7:
        os.kill(pid7, signal.SIGKILL)
        os.waitpid(pid7, 0)
        print("FAIL tui-e2e settings: TUI did not quit")
        fail = 1
    os.close(master7)

# ------------------------------------------ virtual model (alias) round-trip
# A config-declared alias is selectable by name and through the picker, and
# routes end to end through its concrete target (same provider, so the agent's
# own base URL/credential are used). Fresh home so the fixture cannot leak into
# the sessions above.
ahome = os.path.join(ROOT, "build", "tui-e2e-alias-home")
subprocess.run(["rm", "-rf", ahome], check=False)
for sub in ("config", "data", "state"):
    os.makedirs(os.path.join(ahome, sub), exist_ok=True)
os.makedirs(os.path.join(ahome, "config", "abird", "agentc"), exist_ok=True)
with open(os.path.join(ahome, "config", "abird", "agentc", "setup.jsonc"), "w") as f:
    f.write('{"models": {"mock-fast": {"provider": "anthropic", '
            '"model": "claude-sonnet-4-5"}}}\n')
aenv = dict(env, HOME=ahome,
            XDG_CONFIG_HOME=ahome + "/config",
            XDG_DATA_HOME=ahome + "/data",
            XDG_STATE_HOME=ahome + "/state")
master8, slave8 = pty.openpty()
fcntl.ioctl(slave8, _t.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
pid8 = os.fork()
if pid8 == 0:
    os.setsid()
    os.dup2(slave8, 0)
    os.dup2(slave8, 1)
    os.dup2(slave8, 2)
    os.close(master8)
    os.close(slave8)
    os.execvpe(FN, [FN, "--api-key", "test", "--provider", "anthropic",
                    "--model", "claude-sonnet-4-5",
                    "--base-url", "http://127.0.0.1:" + port], aenv)
os.close(slave8)
out8 = b""


def pump8(seconds):
    global out8
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master8], [], [], 0.05)
        if r:
            try:
                out8 += os.read(master8, 65536)
            except OSError:
                return


def wait8_since(off, needle, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if needle in out8[off:]:
            return True
        pump8(0.05)
    return needle in out8[off:]


time.sleep(0.6)
pump8(0.4)
os.write(master8, b"/model mock-fast\n")
if not wait8_since(0, b"model: mock-fast", 3.0):
    print("FAIL tui-e2e alias: /model <alias> did not set")
    fail = 1
if not wait8_since(0, b"alias -> anthropic/claude-sonnet-4-5", 3.0):
    print("FAIL tui-e2e alias: route notice missing")
    fail = 1
# the picker lists the alias row; slice past the notice printed above
n8 = len(out8)
os.write(master8, b"\x03")
pump8(0.2)
os.write(master8, b"/model\n")
if not wait8_since(n8, b"alias -> anthropic/claude-sonnet-4-5", 3.0):
    print("FAIL tui-e2e alias: picker did not list the alias")
    fail = 1
else:
    os.write(master8, b"mock-fast\n")
    if not wait8_since(n8, b"model: mock-fast", 3.0):
        print("FAIL tui-e2e alias: picker did not switch")
        fail = 1
# end-to-end: the alias routes to the mock target
os.write(master8, b"alias round trip\n")
if not wait8_since(n8, b"done", 6.0):
    print("FAIL tui-e2e alias: routed turn did not finish")
    fail = 1
os.write(master8, b"/help\n")
if not wait8_since(n8, b"/model [id|alias], /theme [dark|light]", 3.0):
    print("FAIL tui-e2e alias: /help did not list /model [id|alias]")
    fail = 1
os.write(master8, b"/quit\n")
pump8(0.6)
deadline8 = time.time() + 5
status8 = None
while time.time() < deadline8:
    d8, st8 = os.waitpid(pid8, os.WNOHANG)
    if d8 == pid8:
        status8 = st8
        break
    time.sleep(0.1)
if status8 is None:
    os.kill(pid8, signal.SIGKILL)
    _, status8 = os.waitpid(pid8, 0)
    print("FAIL tui-e2e alias: TUI did not quit")
    fail = 1
elif os.waitstatus_to_exitcode(status8) != 0:
    print("FAIL tui-e2e alias exit code")
    fail = 1
os.close(master8)

srv.terminate()

if fail == 0:
    print("ok   tui-e2e (pty drive, tool round-trip, session persisted, SIGTERM restores)")
sys.exit(fail)
