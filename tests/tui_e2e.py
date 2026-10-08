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
    os.execvpe(FN, [FN, "--api-key", "test", "--provider", "anthropic",
                    "--model", "claude-sonnet-4-5",
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

files = glob.glob(os.path.join(home, "data", "agentc", "sessions", "**", "*.jsonl"),
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
    os.execvpe(FN, [FN, "--api-key", "t", "--offline", "--no-session"], env)
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
# the placeholder row is emitted as the reverse-video caret cell on the first
# glyph followed by the dim muted tail; the tail is the anchor for the SGR
# checks (a background is either an explicit 48;… colour or a 16-colour band)
hint_segs = [s for s in out3.split(b"\r\x1b[2K") if b"eady. Press Ctrl-C" in s]
if not hint_segs:
    print("FAIL tui-e2e empty-state hint missing from the raw stream")
    fail = 1
elif not any(b"\x1b[7m" in s and b"\x1b[2m" in s for s in hint_segs):
    print("FAIL tui-e2e placeholder not dim under the reversed caret")
    fail = 1
elif any(bg_re.search(s) for s in hint_segs):
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
# the composer is two full-width rules (U+2500, or '-' on a dumb terminal)
# bracketing the input; the old highlighted "> " marker must be gone
rules = {"\u2500" * cols, "-" * cols}
text_row = None
rule_rows = []
for idx, row in enumerate(screen.grid):
    line = "".join(row)
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

# Inline mode parks the hardware cursor at the owned region's top-left (the one
# anchor a terminal keeps on a height shrink, so the app's chrome can never be
# pushed into scrollback); the visible caret is the reverse-video cell. The
# scrollback/fullscreen modes still park the hardware cursor on the caret.
if text_row is None or not rule_rows or screen.row != rule_rows[0] or screen.col != 0:
    print("FAIL tui-e2e inline cursor not parked at the region top-left",
          screen.row, screen.col)
    print(screen.text())
    fail = 1

# The rules must sit on the terminal's own background while the status line
# still paints the theme band: inspect the SGR of the clear-delimited rows.
rule_segs = [s for s in out3.split(b"\r\x1b[2K")
             if any(g * cols in s.decode("utf-8", "replace")
                    for g in ("\u2500", "-"))]
if not rule_segs:
    print("FAIL tui-e2e no full-width composer rule rows in the raw stream")
    fail = 1
elif any(bg_re.search(s) for s in rule_segs):
    print("FAIL tui-e2e composer rule row painted a background")
    fail = 1
status_segs = [s for s in out3.split(b"\r\x1b[2K") if b"ready" in s]
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
if not wait_for(b"/model [id], /theme [dark|light]", 3.0):
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
def menu_spawn(mode, mcols, mrows):
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
        os.execvpe(FN, [FN, "--api-key", "t", "--offline", "--no-session",
                        "--tui-mode", mode], env)
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


def row_hits(screen, needle):
    return [i for i, row in enumerate(screen.grid) if needle in "".join(row)]


# inline: the menu sits below the composer's bottom rule and above the status
master_m, pid_m = menu_spawn("inline", 80, 24)
text = menu_collect(master_m, 0.8)
os.write(master_m, b"/")
text += menu_collect(master_m, 0.5)
screen_m = Screen(80, 24).feed(text)
if "/model" not in screen_m.text() or "/quit" not in screen_m.text():
    print("FAIL tui-e2e inline menu did not open on /")
    print(screen_m.text())
    fail = 1
rules_m = row_hits(screen_m, "\u2500" * 80)
menu_m = row_hits(screen_m, "/model")
status_m = row_hits(screen_m, "tok:0/0")
if (len(rules_m) != 2 or len(menu_m) != 1 or len(status_m) != 1
        or not (rules_m[1] < menu_m[0] < status_m[0])):
    print("FAIL tui-e2e inline menu placement", rules_m, menu_m, status_m)
    print(screen_m.text())
    fail = 1

# prefix filtering narrows the list to /model
os.write(master_m, b"mo")
text += menu_collect(master_m, 0.4)
screen_m = Screen(80, 24).feed(text)
if "/model" not in screen_m.text() or "/help" in screen_m.text():
    print("FAIL tui-e2e menu prefix filter")
    print(screen_m.text())
    fail = 1

# Escape dismisses without touching the text and without leaving the menu
# behind (the composer keeps /mo, no entry row remains on screen).
os.write(master_m, b"\x1b")
text += menu_collect(master_m, 0.5)
screen_m = Screen(80, 24).feed(text)
if ("/model" in screen_m.text() or "/help" in screen_m.text()
        or "/clear" in screen_m.text()):
    print("FAIL tui-e2e menu left behind after dismissal")
    print(screen_m.text())
    fail = 1
if "/mo" not in screen_m.text():
    print("FAIL tui-e2e Escape touched the composer text")
    print(screen_m.text())
    fail = 1
menu_kill(master_m, pid_m)

# fullscreen: the menu sits above the composer's top rule
master_f, pid_f = menu_spawn("fullscreen", 80, 24)
text = menu_collect(master_f, 0.7)
os.write(master_f, b"/")
text += menu_collect(master_f, 0.5)
screen_f = Screen(80, 24).feed(text)
rules_f = row_hits(screen_f, "\u2500" * 80)
menu_f = row_hits(screen_f, "/model")
if not menu_f or not rules_f or menu_f[0] > rules_f[0]:
    print("FAIL tui-e2e fullscreen menu placement", rules_f, menu_f)
    print(screen_f.text())
    fail = 1
menu_kill(master_f, pid_f)

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
    os.execvpe(FN, [FN, "--api-key", "t", "--offline", "--no-session"], env)
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

# ------------------------------------------------ inline resize storm
# A terminal resize reflows the screen and invalidates the inline live region's
# absolute rows. The app must erase the rows it owned (from the parked cursor,
# before trusting the new geometry) and re-anchor the region at the bottom; the
# storm asserts that no chrome row or committed transcript line is left behind
# or duplicated in the reconstructed screen + scrollback.
import fcntl as _fcntl
import struct as _struct
import termios as _termios


def storm_spawn(args, cols, rows, prelude=None):
    m, s = pty.openpty()
    _fcntl.ioctl(s, _termios.TIOCSWINSZ, _struct.pack("HHHH", rows, cols, 0, 0))
    p = os.fork()
    if p == 0:
        os.setsid()
        os.dup2(s, 0)
        os.dup2(s, 1)
        os.dup2(s, 2)
        os.close(m)
        os.close(s)
        if prelude:
            script = prelude + '; exec "$0" "$@"'
            os.execvpe("/bin/sh", ["sh", "-c", script, FN] + args, env)
        else:
            os.execvpe(FN, [FN] + args, env)
    os.close(s)
    return m, p


def storm_drain(m, seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([m], [], [], 0.05)
        if r:
            try:
                out += os.read(m, 65536)
            except OSError:
                break
    return out


def storm_settle(m, quiet=0.04, cap=0.6):
    """Read until the app has been quiet for `quiet` seconds (or `cap`). A
    frame is one atomic write, so a quiet gap means the next resize is applied
    to a complete old-geometry screen rather than to bytes still in flight."""
    out = b""
    last = time.time()
    start = time.time()
    while time.time() - start < cap:
        r, _, _ = select.select([m], [], [], 0.01)
        if r:
            try:
                out += os.read(m, 65536)
            except OSError:
                break
            last = time.time()
        elif time.time() - last >= quiet:
            break
    return out


def storm_resize(screen, m, cols, rows, settle=0.6):
    """Resize the pty and the emulator together, then replay the app's reply.
    The emulator is resized before the post-resize bytes are fed: a real
    terminal reflows first and only then does the app react, so the app's
    cursor-relative erase must be replayed against the reflowed screen."""
    screen.feed(storm_settle(m))
    _fcntl.ioctl(m, _termios.TIOCSWINSZ, _struct.pack("HHHH", rows, cols, 0, 0))
    screen.resize(cols, rows)
    screen.feed(storm_settle(m, cap=settle))


def storm_quit(m, p, timeout=5.0):
    try:
        os.write(m, b"/quit\n")
    except OSError:
        pass
    end = time.time() + timeout
    while time.time() < end:
        done, st = os.waitpid(p, os.WNOHANG)
        if done == p:
            return st
        storm_drain(m, 0.05)
    os.kill(p, signal.SIGKILL)
    _, st = os.waitpid(p, 0)
    return st


STORM = [(48, 16), (120, 40), (30, 8), (200, 60), (40, 10), (80, 24)]
EXTREMES = [(1, 1), (1, 8), (8, 1), (2, 2), (3, 3), (80, 24)]
HINT = "Ready. Press Ctrl-C once to clear, twice to exit"


def rule_rows(screen):
    live = ["".join(r).rstrip() for r in screen.grid]
    return [r for r in live + screen.scrollback
            if len(r) >= 1 and set(r) == {"\u2500"}]


def scrollback_rule_rows(screen):
    return [r for r in screen.scrollback if len(r) >= 1 and set(r) == {"\u2500"}]


# 1. idle: the empty-state chrome must never reach scrollback, however often the
# terminal resizes. The shell marker above the region must survive.
m, p = storm_spawn(["--api-key", "t", "--offline", "--no-session"], 80, 24,
                   prelude="echo SHELL-MARKER-IDLE")
scr = Screen(80, 24).feed(storm_drain(m, 0.8))
for c, r in STORM + EXTREMES:
    storm_resize(scr, m, c, r)
scr.feed(storm_settle(m))
scr.feed(storm_settle(m))
if scr.count_scrollback_rows_containing(HINT):
    print("FAIL tui-e2e storm: hint left in scrollback",
          scr.count_scrollback_rows_containing(HINT))
    print(scr.all_text())
    fail = 1
if scr.count_all_rows_containing(HINT) != 1:
    print("FAIL tui-e2e storm: hint not exactly once",
          scr.count_all_rows_containing(HINT))
    print(scr.all_text())
    fail = 1
rules = rule_rows(scr)
if len(rules) != 2:
    print("FAIL tui-e2e storm: composer rules", len(rules))
    print(scr.all_text())
    fail = 1
if scrollback_rule_rows(scr):
    print("FAIL tui-e2e storm: composer rules left in scrollback",
          len(scrollback_rule_rows(scr)))
    print(scr.all_text())
    fail = 1
if scr.count_all_rows_containing("SHELL-MARKER-IDLE") != 1:
    print("FAIL tui-e2e storm: shell history marker lost/duplicated")
    print(scr.all_text())
    fail = 1
st = storm_quit(m, p)
if os.waitstatus_to_exitcode(st) != 0:
    print("FAIL tui-e2e storm: idle TUI exit code")
    fail = 1
os.close(m)

# 2. committed transcript: drive a real turn against the mock provider and
# resize during the stream; every committed line must appear exactly once, in
# order, in the final reconstruction.
m, p = storm_spawn(["--api-key", "test", "--provider", "anthropic",
                    "--model", "claude-sonnet-4-5",
                    "--base-url", "http://127.0.0.1:" + port],
                   80, 24, prelude="echo SHELL-MARKER-TURN")
scr = Screen(80, 24)
scr.feed(storm_drain(m, 0.8))
os.write(m, b"/help\n")
scr.feed(storm_drain(m, 0.5))
os.write(m, b"read the allocator\n")
# the mock provider streams back almost immediately; resize right away so the
# SIGWINCH lands while the turn (and its SSE deltas) is in flight
for c, r in [(60, 14), (100, 30), (44, 12)]:
    storm_resize(scr, m, c, r)
scr.feed(storm_settle(m))
for c, r in EXTREMES + [(80, 24)]:
    storm_resize(scr, m, c, r)
scr.feed(storm_settle(m))
scr.feed(storm_settle(m))
marked = ["SHELL-MARKER-TURN", "/model [id], /theme [dark|light]",
          "> read the allocator", "done"]
positions = []
for needle in marked:
    count = scr.count_all_rows_containing(needle)
    if count != 1:
        print("FAIL tui-e2e storm: committed line count", needle, count)
        print(scr.all_text())
        fail = 1
    idx = scr.all_text().find(needle)
    positions.append(idx)
if positions != sorted(positions):
    print("FAIL tui-e2e storm: committed lines out of order", positions)
    fail = 1
if scr.count_scrollback_rows_containing("tok:0/0"):
    print("FAIL tui-e2e storm: footer left in scrollback")
    fail = 1
rules = rule_rows(scr)
if len(rules) != 2:
    print("FAIL tui-e2e storm: composer rules after turn", len(rules))
    print(scr.all_text())
    fail = 1
if scrollback_rule_rows(scr):
    print("FAIL tui-e2e storm: composer rules left in scrollback after turn",
          len(scrollback_rule_rows(scr)))
    fail = 1
st = storm_quit(m, p)
if os.waitstatus_to_exitcode(st) != 0:
    print("FAIL tui-e2e storm: turn TUI exit code")
    fail = 1
os.close(m)

srv.terminate()

if fail == 0:
    print("ok   tui-e2e (pty drive, tool round-trip, session persisted, SIGTERM restores)")
sys.exit(fail)
