# 40 — Terminal UI

The terminal UI is a small immediate-mode renderer over one cell grid and one
event stream. It targets production-grade polish at a fraction of the code: one
frame is one contiguous byte buffer delivered in one terminal write, layout and
diffing allocate no per-cell or per-row storage, and the transcript stays in the
terminal's own scrollback under the default mode.

Sources: `src/tui/tui.c` (layout, input routing, commands, main loop),
`src/tui/render.c` (cell grid and differential writer), `src/tui/term.c`
(terminal lifecycle and the two backends), `src/tui/editor.c` (composer),
`src/tui/input.c` (escape parser), `src/tui/markdown.c` (streaming markdown),
`src/tui/components.c` (chat viewport, cards, footer), `src/tui/theme.c`,
`include/status.h` and `src/core/status_builtin.c`.

## 1. Terminal lifecycle (`src/tui/term.c`)

`term_open_tty()` queries the terminal size (`os_tty_size`); a zero or negative
size falls back to 80×24. `term_open_memory()` is the scripted backend the golden
tests drive: frame bytes append to an `AgcBuf` and input comes from a queue, so
the whole UI runs without a tty.

### 1.1 Startup

`term_enter_mode()` runs once:

1. `os_tty_raw(0, &saved)` saves the untouched termios and switches to raw mode
   (POSIX); Windows uses `SetConsoleMode` with VT processing enabled.
2. Bracketed paste is enabled (`\x1b[?2004h`) and the cursor is hidden
   (`\x1b[?25l`).
3. In fullscreen mode the alternate screen is entered (`\x1b[?1049h`); inline and
   scrollback never enter it.
4. The size is tracked and the resize protocol is armed.

Fullscreen frames wrap the differential repaint in a synchronized-update bracket
(`\x1b[?2026h` … `\x1b[?2026l`); terminals that do not implement it ignore the
sequences. `term_leave` also emits the closing `\x1b[?2026l`, which is
idempotent, so a restore is safe even if a signal lands in the middle of a frame.

### 1.2 Presentation modes and ownership

One cell grid and one event stream back three modes. `inline` is the default
everywhere the mode is chosen (`--tui-mode`, its `auto` alias, and the `tui_mode`
config key).

| mode | owns | commits | resize |
|---|---|---|---|
| `scrollback` | nothing; the live tail is redrawn by moving up over the previous frame | finished rows are printed once and never redrawn | erase the drawn rows, reset bookkeeping, rebuild grids |
| `inline` (default) | a rectangular region at the bottom, `h = transcript tail + queue + editor + menu + footer` rows, recomputed every frame | whole transcript blocks rendered once at the region's top edge, then scrolled up into real scrollback; a live block taller than the region is shown from its tail until it finishes | erase the owned rows, re-anchor at the new bottom, repaint the whole region with absolute positioning in one write |
| `fullscreen` | the alternate-screen grid | differential repaint; no terminal scrollback involvement | grid resize plus a full diff |

**Ownership.** In inline mode the app owns exactly the bottom `h` rows. Every
owned row is painted with an absolute cursor move (`CSI row;1H`) and a full row
clear, so a repaint cannot scroll the screen by accident. Shell history and
already-committed transcript rows above the region are never touched. Committing
is the only path that scrolls: committed blocks are rendered once, printed at the
region's top edge, and the screen is scrolled by the printed count.

**Parked cursor.** Between inline frames the hardware cursor is parked at the
region's top-left and kept hidden; the editor's reverse-video cell is the visible
caret, so exactly one cursor is on screen. The parked top-left is the one anchor
a terminal preserves on a height shrink: every owned row sits below it, so the
terminal discards or scrolls the app's chrome instead of pushing it into
scrollback. `--tui-mode fullscreen` and `scrollback` instead park the hardware
cursor on the caret cell and show it.

### 1.3 Shutdown and crash safety

`term_leave` is idempotent and runs on every exit path. It resets any pushed
terminal background (`OSC 111`), writes sync-off + cursor-on + paste-off +
alt-off (alt only when the alternate screen was entered), and restores the saved
termios.

- **Normal exit.** `agentc_tui_run` leaves the mode (inline/scrollback flush the
  remaining transcript), calls `term_leave`, and frees the UI.
- **Fatal path.** `agentc_atexit(tui_emergency_restore)` is registered, so
  `agentc_die` (for example out of memory) restores the terminal before exit.
- **Signals.** `os_sig_install` installs `tui_signal` for `SIGHUP`, `SIGINT`,
  `SIGQUIT`, `SIGABRT`, `SIGBUS`, `SIGFPE`, `SIGSEGV` and `SIGTERM`.
  `tui_signal` only calls `term_leave` and `os_exit(128 + sig)`; it allocates
  nothing.
- **Lost terminal.** A `OS_POLLHUP`/`OS_POLLERR` on stdin quits the loop.

### 1.4 Resize protocol

Resize is detected by polling the terminal size each loop iteration and applied
through one shared path (`tui_apply_resize`) so the poll-time and mid-frame cases
cannot drift. In inline mode `inline_erase_owned()` runs before any new geometry
is trusted:

1. The erase starts at the parked region top and clears from there to the end of
   the screen (`\r\x1b[J`); no absolute row is recomputed from the old width and
   no row count is guessed. Anything the previous geometry owned — a reflow that
   changed the region's height, or an earlier partial erase — is below the top, so
   one erase-to-end covers it, while rows above the region (banner, shell history)
   are untouched.
2. The next frame recomputes `h`, re-anchors the region at the new bottom and
   repaints it in one write. A shrink (e.g. a picker closing) uses the same erase
   before repainting the shorter region, so the trailing rows and any theme band
   there cannot be left behind.

The committed transcript boundary is tracked in **whole blocks**, not rows:
`chat_block_height()` is measured at the current width and the boundary is
recomputed after a width change. A partial-block boundary would tie the count to
one width's layout and reprint or skip reflowed rows. A live block taller than
the region is therefore shown from its tail (sticky bottom) and committed in one
piece when it finishes.

A resize that lands while a frame is composing is discarded: `tui_check_resize`
records the new size and the frame checks the geometry generation before writing,
rolling back the committed-boundary and test-mirror bookkeeping it advanced, then
applies the erase and re-anchor before the next attempt.

## 2. Render pipeline (`src/tui/tui.c`, `src/tui/render.c`)

**One frame, one write.** The renderer composes a frame into one contiguous byte
buffer (`AgcBuf`) and the backend performs a single `os_write`; no text is
written to the terminal from the grid directly, and no vectored writes are used.
Layout and diffing allocate no per-cell, per-row or per-glyph storage. Grids are
allocated once and reallocated only on resize (capped at `GRID_MAX_COLS` 400 ×
`GRID_MAX_ROWS` 200); the only allocation on the frame path is the frame byte
buffer itself.

`Cell` carries `cp`, one attached combining mark (`comb`), `attrs`, `fg` and
`bg` (theme slots). Two sentinels drive drawing: `CELL_CONT` marks the second
column of a wide glyph and `CELL_INVALID` marks a cell that has never been drawn
and forces a redraw.

Frames are driven by a dirty flag. The main loop polls with a 16 ms timeout;
while a run is active a frame is rendered at most every 100 ms, so streamed
deltas coalesce instead of painting per token. Deltas append to per-message
buffers that are flushed into the transcript each frame; when the agent retries
an attempt that already streamed, the `message_reset` event rolls the chat back
to the block watermark recorded at the matching `message_start` and drops the
coalesced buffers, so a retried answer replaces the abandoned draft instead of
appending to it. The watermark is cleared on `message_end`.

### 2.1 Fullscreen differential writer

`render_diff(prev, next, out, theme)` rewrites only changed rows:

1. Compare each row's cells by `cp`, `comb`, `attrs`, `fg` and `bg`; record the
   first and last changed column; a row with no change is skipped.
2. Emit one cursor move (`CSI row;1H`) per changed row.
3. Emit SGR only when the attribute/color tuple changes, then the cell's glyph
   and its combining mark.
4. Emit `\x1b[0m\x1b[K` to clear a changed row's tail when the last changed
   column is not the final column; the reset keeps a trailing reverse-video
   caret or menu selection from bleeding into the erased cells.

The fullscreen frame then hides the cursor, opens the synchronized-update
bracket, writes the diff, resets attributes, positions and shows the hardware
cursor on the caret cell, closes the bracket, and swaps the `prev`/`cur` grids.

### 2.2 Append writer (`scrollback` and commit path)

`render_rows_ansi` emits rows `[y0, y1)` as ANSI lines: `\r\x1b[2K`, then the
styled content, then `\r\n`. Trailing blanks are trimmed, except that a
reverse-video cell (the caret fallback) and a cell with a themed background band
(the status line or a tool card) extend the row to its right edge and are never
trimmed. Autowrap is disabled (`\x1b[?7l`) around each repaint so a full-width
row cannot leave the terminal in a pending-wrap state; it is restored
(`\x1b[?7h`) before the atomic write ends.

The `scrollback` frame moves the cursor up over the previous live region, clears
and re-emits it, then parks the hardware cursor on the caret cell. Finished rows
are printed once through `scrollback_print_rows`, which commits in bounded
chunks (at most `GRID_MAX_ROWS`) and advances the committed boundary only over
rows actually emitted.

### 2.3 Inline writer

The inline frame composes the live region (transcript tail + queue + editor +
menu + footer) and then writes, in one cursor-relative sequence: hide the cursor,
disable autowrap, clear the old region when it shrank, commit new transcript rows
at the cursor with newline mode so they flow above and push the region down,
repaint the whole region with absolute positioning, move back up over the drawn
rows, park the hidden cursor at the region's top-left (`CSI 1G`), and restore
autowrap. Because everything is relative to the parked anchor, the region follows
the transcript rather than being pinned to the terminal bottom.

### 2.4 Caret

The composer paints the cell at the caret reverse-video (a space when the caret is
past the last character) and the frame positions the real terminal cursor on that
same cell in fullscreen/scrollback, or parks the hidden cursor at the region top
in inline mode. The reverse cell is the fallback: the in-memory test backend has
no hardware cursor, and a terminal that ignores `\x1b[?25h` still shows a caret.
A wide glyph reverses as a whole because the SGR applies to both terminal
columns; a combining mark reverses the cell it attaches to; a caret on a newline
parks at the end of the preceding visual row. A terminal that reports zero width
is clamped to the grid's first cell.

## 3. Width and UTF-8 handling (`src/tui/render.c`, `src/tui/editor.c`)

- **Width table.** `agentc_wcwidth()` is a hand-maintained table: combining-mark
  ranges return 0, as do zero-width format characters (the Unicode `Cf` set plus
  soft hyphen and the Mongolian vowel separator), C0 controls and DEL
  return 0, and East Asian Wide/Fullwidth ranges return 2. Anything outside the
  table falls back to width 1, the conservative choice: a miscounted single-width
  glyph can shift the caret by one column, while treating a narrow glyph as wide
  leaves a visible hole.
- **Wide glyphs and combining marks.** `grid_put()` consumes two cells for a
  wide glyph and marks the second `CELL_CONT`; a zero-width mark attaches to the
  base cell (skipping a `CELL_CONT` continuation cell), one mark per cell. A wide
  glyph that would cross the right edge is blanked instead of split.
- **Malformed UTF-8.** `utf8_decode()` in the renderer, `editor_decode()` in
  the editor and `utf8_cp_at()` in the markdown wrapper all validate shape *and*
  range: an invalid lead, truncated sequence, bad continuation, overlong form,
  surrogate (U+D800..DFFF) or scalar above U+10FFFF yields U+FFFD and consumes
  one byte. Every pass that measures or draws the buffer uses the same rule, so
  the visual-row count and the rendered rows cannot disagree on malformed input
  (and a surrogate is never re-encoded to the terminal).
- **Tabs.** Tabs are expanded to the next 4-column stop when text enters the
  editor, and `grid_put()` expands them for other grid users, so the wrap, caret
  and clip math and the drawn glyphs agree.
- **Display width.** `agentc_text_width()` sums `agentc_wcwidth()` over a string;
  the footer uses it to right-align the right slot by display columns rather than
  byte count.

## 4. Editor and readline keymap (`src/tui/editor.c`)

The composer is bracketed by two full-width dim horizontal rules (U+2500, or `-`
when `TERM=dumb` or `AGENTC_ASCII` is set) and carries no prompt marker. Text
starts at column 0; one column at the right edge is reserved for the caret past
the last character; the rules and the placeholder are chrome only and never enter
the buffer, the kill ring or scrollback. An empty composer shows the placeholder
"Ready. Press Ctrl-C once to clear, twice to exit" on the input row, drawn dim +
muted before the caret pass, clipped rather than wrapped, and shown only while
the transcript is empty and no run is in flight.

The document is a flat `AgcBuf` with a byte cursor and anchor; the kill ring,
history and collapsed paste bodies are separate owned buffers. Multiline input is
`Alt+Enter`; `Enter` submits.

| Feature | Behaviour |
|---|---|
| Kill ring | `Ctrl+K/U/W/Y`, `Alt+D`, `Ctrl+Delete`, `Alt+Backspace`, feeding `Ctrl+Y` |
| Word motion | `Alt+B/F`, `Ctrl+←/→` |
| History | `~/.local/state/agentc/history` (deduped, capped at 1000 lines) |
| Bracketed paste | pastes over 10 lines collapse to `[Pasted text #n +N lines]`, expanded into the message on submit |
| `@file` completion | `Tab` completes the `@` token by opening its directory with `os_getdents`, scoring entries prefix-first then as a subsequence, and substituting the best single match (a directory gains a trailing `/`); capped at `COMPLETE_MAX_ENTRIES` 512 entries scanned |

Readline/emacs keymap:

| key | action |
|---|---|
| `Ctrl+A`, `Home` | start of line (`Ctrl+Home`: start of input) |
| `Ctrl+E`, `End` | end of line (`Ctrl+End`: end of input) |
| `Ctrl+B`, `Left` | character back |
| `Ctrl+F`, `Right` | character forward |
| `Alt+B`, `Ctrl+Left` | word back |
| `Alt+F`, `Ctrl+Right` | word forward |
| `Ctrl+W`, `Alt+Backspace` | kill word backward |
| `Alt+D`, `Ctrl+Delete` | kill word forward |
| `Ctrl+U` | kill from the caret to the start of the line |
| `Ctrl+K` | kill from the caret to the end of the line |
| `Ctrl+Y` | yank the kill buffer at the caret |
| `Ctrl+D` | delete the character under the caret; on empty input, quit (EOF) |
| `Ctrl+C` | clear the composer on the first press; a second press within 1 s quits |
| `Delete` | delete the character under the caret |
| `Ctrl+T` | transpose the characters around the caret |
| `Ctrl+O` | toggle the last live tool card collapsed/expanded |
| `PageUp`/`PageDown` | page the transcript viewport by one screen less a row |

Word boundaries are the readline default: a word is a run of `[A-Za-z0-9_]` plus
any non-ASCII byte, so whitespace and punctuation delimit words. The same rule
drives movement, the kill-word bindings and the kill buffer.

The input parser (`src/tui/input.c`) is an incremental state machine (ground,
ESC, CSI, SS3, paste marker) with a 50 ms inter-byte timeout, so a lone ESC
aborts immediately. ESC is a prefix, not an action: `ESC` + key emits the
Alt/CSI key on the next byte, and only an ESC with no follower inside the window
becomes `K_ESC`. It accepts the kitty protocol's `CSI codepoint;mods u` form and
the legacy single-parameter modifier arrows (`CSI 5D`, `CSI 3A`). Mouse input is
disabled so native selection/copy keeps working.

## 5. Streaming transcript, markdown and tool cards

Streaming deltas are typed events. Text and thinking deltas append to per-run
buffers (`pend_text`, `pend_think`) and are flushed once per frame in arrival
order, so a long answer triggers one markdown append per frame rather than per
token. Thinking deltas are gated by the configured level: a reasoning model can
stream reasoning content with the level `off`, and that text stays out of the
view so the footer (`think:off`) and the visible output agree; the transcript
still records the block for replay.

`src/tui/markdown.c` parses line by line into blocks (`MD_PARA`, `MD_HEAD`,
`MD_BULLET`, `MD_CODE`). Appending re-renders only the trailing incomplete block;
completed blocks keep their wrapped, styled rows. Inline styling covers bold
(`**`), italic (`*`/`_`) and code (`` ` ``); fenced code buffers verbatim until
the closing fence. Rendering happens at a fixed width, so a resize re-renders all
blocks once.

Tool cards are `CHAT_TOOL` blocks. A card starts on the tool-execution-start
event and ends on the tool-execution-end event; an end event with no matching
running card creates one. The header shows the tool name, a truncated argument
preview, a spinner and elapsed time while running, and `ok`/`err` plus duration
when finished. The body shows the tail of the output buffer (three lines
collapsed, ten expanded) with a `... (+N lines)` marker; `Ctrl+O` toggles the
last live card. Output is sliced from the end of the `AgcBuf`, so no per-line
allocation happens. For the `edit` tool, `+`/`-`/`@@` lines get diff colors.

Each card paints its own rows as a full-width background band by outcome: muted
grey while running or when no end event arrived (for example after an abort),
muted green for a completed success, muted red for a reported error. The band is
the base layer and every foreground tint (header, status, diff, body) draws on
top of it; the trailing gap row between cards is not part of either card and
keeps the terminal background, so adjacent bands stay separate. The tint defaults
follow the pi palette (see §8).

The chat viewport is virtual-scrolled over the block vector: it renders only the
visible rows, sticks to the bottom unless the user scrolled up, and anchors the
reading position while new content arrives. `PageUp`/`PageDown` page by the
viewport height less one row. Pending steering/follow-up messages appear in a
one-row queue strip above the composer. `Esc` aborts the run and returns queued
messages to the editor; `Ctrl+C` clears the composer and arms the 1 s
double-press quit window.

## 6. Slash-command menu

Typing `/` at the start of an empty composer opens the command menu. It stays
open while the composer holds an unterminated command word (leading `/`, no
space or newline yet) and filters by case-sensitive prefix as the word grows
(`/mo` narrows to `/model`). A word that matches nothing closes the menu; a
space ends the word and starts the argument phase.

Entries come from one ordered set of sources: the built-in `tui_commands` table
in `src/tui/tui.c` (the same table `tui_command()` dispatches through), then the
invocable-prompt registry (`agentc_prompts_list()`), then `agentc_ext_commands()`.
Precedence follows that order, and the `skill:` prefix is reserved, so the menu
never advertises an entry the dispatcher would not reach.

While the menu is open it consumes exactly `Up`/`Down`/`Tab`/`Enter`/`Esc`.
`Tab` completes the selected command plus a trailing space; `Enter` completes and
submits through the same `tui_accept()` path as the editor; `Esc` dismisses the
menu without touching the text or an in-flight run. Every other key falls through
to the editor, and with the menu closed `Up`/`Down` keep their history-navigation
meaning. Placement is above the composer in fullscreen and below it inline; the
list is capped to the available rows and scrolled so the selection stays visible,
and is dropped entirely if not even one row is free. The selected row is drawn as
one full-width reverse band (the gap between the name and its description is
filled too), so a row reads as a single selection.

### 6.1 Model, thinking and session pickers

Every modal picker shares one list mechanic (`src/tui/picklist.h`): substring
filtering, wrap-around `Up`/`Down` (top wraps to bottom and back), `PageUp`/
`PageDown` paging, and modal `Enter`/`Esc`/`Ctrl+C`. Row names, descriptions and
session paths are owned by the TUI, so no catalog or session pointer can dangle.
The selected row is one full-width reverse band. While a picker is open it owns
the keyboard; every key is swallowed.

- `/model` (no argument): the runtime catalog for the current provider,
restricted to the current model's wire (`agentc_model_filter`) so the two
`openai` rows never mix, with the current model marked; `Enter` switches with
`agentc_agent_set_model()`. `/model <id>` still sets directly.
- `/thinking` (no argument): off/low/medium/high with the live level marked
(read from the agent); `Enter` sets `agentc_agent_set_thinking()`. `/thinking
<level>` still sets directly. The default level is **medium** when neither
`--thinking` nor `default_thinking` is set.
- `/resume` and `/continue`: stored sessions, newest first
(`agentc_session_age_label()` + the opening line from
`agentc_session_summary()`); `Enter` switches via `AgcTuiApp::resume_session`
(`agentc_mode_resume_session`).

`--continue`/`--resume` on a terminal open the same session picker before the
first prompt (`AgcTuiApp::pick_session_on_start`); the mode opens the newest
session first, so `Esc` keeps it. Scripted, print and non-tty runs keep resuming
the newest (the tty gate is what distinguishes them).

### 6.2 A clean mid-run switch

`/resume`/`/continue` work at any point. While a run is in flight the command
sets `switch_pending` and aborts the turn; the poll hook keeps processing keys
until the submit unwinds. Only then (`!running`) does the main loop open the
picker, so the swap can never race an executing tool: the abort already
cancelled the in-flight request, the tool driver killed running jobs, and the
HTTP poll hook returned. `Enter` then calls `AgcTuiApp::resume_session`, which
replaces the agent transcript and rebinds persistence; the TUI clears its view
and commit boundary. `/new` follows the same rule (refuse while running).

### 6.3 /compact and /new

`/compact` runs a manual `agentc_agent_compact()` and reports the outcome
(compacted / nothing / error); the COMPACT event still persists the checkpoint
through `AgcTuiApp::on_compact`. `/new` starts a fresh session through
`AgcTuiApp::new_session` (`agentc_mode_new_session`): close the file, create and
rebind a new one, clear the agent transcript, then clear the view. Without app
services (tests, library use) `/new` only clears the view.

## 7. Status line segment registry (`include/status.h`, `src/core/status_builtin.c`)

The footer is a list, not a fixed string. Every provider — built-in or extension —
registers through `agentc_status_register()` and returns
`AgcExtStatusSegment` values (`include/agentc_ext.h`): a slot (`LEFT`/`RIGHT`), a
priority, a style and plain UTF-8 text. Built-ins are an ordinary provider: the
TUI hands them an `AgcStatusBuiltin` state snapshot and they flow through the
same table as an extension's status callback. There is no privileged path into
`comp_footer()`.

The registry validates and copies segments, then `agentc_status_snapshot()`
sorts them by `(slot, priority, registration order)`. Bounds:

| Bound | Value |
|---|---|
| live providers | 16 (`AGENTC_STATUS_MAX_PROVIDERS`) |
| segments per snapshot | `AGENTC_STATUS_MAX_SEGMENTS` 24 |
| segments per provider | `AGENTC_STATUS_MAX_PROVIDER_SEGMENTS` 8 |
| provider text arena | `AGENTC_STATUS_ARENA_CAP` 1024 bytes |
| text kept per segment | `AGENTC_STATUS_TEXT_MAX` 192 bytes, NUL included |

`comp_footer()` owns everything visible: it joins one slot with ` | `,
right-aligns the right slot, and clips both slots to the terminal width (the
right slot wins a full row; the left slot is clipped first). A chatty provider
can never wrap the row or move it. `tui_status_sync()` rebuilds the cached
snapshot only when `agentc_status_version()` changes, never once per frame, so an
idle footer runs no provider code. A provider over the ~2 ms budget is disabled
for the rest of the process with a log line; a segment with an unknown slot or
style, a short `struct_size`, empty text, or an embedded control character is
dropped individually so one bad segment cannot hide a good one.

Styles map to theme slots: `DIM`/`BOLD` to attributes,
`ACCENT`/`WARN`/`ERROR`/`OK` to `TH_ACCENT`/`TH_WARN`/`TH_ERR`/`TH_OK`, and an
unadorned segment to `TH_MUTED` on the status line's `TH_BG` band. Providers only
classify text; they never see escape sequences or cursor movement.

## 8. Themes (`src/tui/theme.c`)

Built-ins are `dark` and `light` (dark and light low-saturation palettes).
`system` picks a base from the terminal: `$COLORFGBG`'s last field (0–6 and 8
dark, 7 and 9–15 light, anything unparseable falls back to dark), else dark.

Named themes resolve from `themes/<name>.jsonc` under the config dir, the trusted
project's `<cwd>/.agentc/themes`, and each `resources_discover` theme root (most
specific first). The stem is the theme name (`[A-Za-z0-9._-]{1,64}`, no `.` or
`..`; invalid stems are skipped with a warning). The schema is `base`
(`dark`/`light`) plus `fg`, `bg`, `muted`, `accent`, `ok`, `warn`, `err`, `user`,
`assistant`, `thinking`, `tool`, `diff_add`, `diff_del`, `tool_ok_bg`,
`tool_err_bg`, `tool_bg`. Precedence is slot by slot, most specific last: built-in
base, the `<config>/theme.jsonc` `base`, then the named file's `base`; the config
file's slots, then the named file's slots. `/theme <name>` applies a name
at runtime and re-reads the file; `/theme` alone toggles dark/light. Project
themes resolve only under the app's already-resolved trust verdict, which the app
passes to `agentc_tui_run`; the TUI never re-derives trust.

Colors are 24-bit by default; `TERM`/`COLORTERM` detection downgrades to the
256-color cube or the 16 ANSI colors. At 256 colors a dark, near-neutral tint
(the base and the muted tool backgrounds) resolves to the nearest greyscale-ramp
entry rather than the 6×6×6 cube, whose first non-zero level is a far lighter
grey. A fixed `light`/`dark` palette owns its background, so the TUI pushes
`TH_BG` to the terminal with `OSC 11` on startup and on every `/theme` apply and
resets it with `OSC 111` on exit; `system` and named themes keep the terminal's
own background.

`TH_NO_BG` is the pseudo-slot for "leave the terminal background", emitted as
`SGR 49`. It is the default for all chrome except the status line and tool cards,
which are the only two surfaces that paint theme backgrounds. The tool-card
background tints (`tool_ok_bg`/`tool_err_bg`/`tool_bg`) default to the
[pi](https://github.com/earendil-works/pi) palette
(`#283228`/`#3C2828`/`#282832` dark, `#E8F0E8`/`#F0E8E8`/`#E8E8F0` light).

## 9. Terminal-injection sanitizer invariants

Model output, tool output and extension-provided text are untrusted. The
following invariants hold on the render path:

1. **Only genuine combining marks attach.** `grid_put()` attaches a zero-width
   codepoint to the previous cell only when `agentc_is_combining()` is true. Any
   other zero-width codepoint that is a C0 control or DEL is replaced with a
   space; other zero-width format characters are dropped and never drawn.
2. **The emitter refuses control bytes.** `emit_cell_cp()` never writes a
   codepoint below `0x20` or equal to `0x7F`; such a cell is emitted as a space.
   An attached combining mark is emitted only when it is `>= 0x20` and not
   `0x7F`. This is defence in depth behind the grid.
3. **Tool-card headers are sanitized.** The argument preview replaces newline,
   carriage return and tab with a space before it reaches the grid.
4. **Providers supply plain text, never control.** A status provider returns
   bytes and a style only; the registry drops a segment whose text contains a
   control character (`< 0x20` or `0x7F`), copies and clips the rest, and the
   footer draws it through `grid_put_clip()`. Providers cannot emit escape
   sequences or move the cursor.
5. **Markdown and the editor share the sanitizer.** All transcript text and
   composer text reach the terminal through `grid_put()`/`emit_cell_cp()`, so the
   same rules apply to streamed markdown and `@file` completions.
6. **A wide glyph at the right edge is blanked**, never split into a stray
   continuation cell.

Together these prevent stray C0/DEL bytes from model or tool output from
reaching the terminal as raw escapes (for example title or clipboard hijacks).

## 10. Headless modes

The same typed event bus drives interactive TUI, `-p` print, `--mode json` and
`--mode rpc`. Print mode writes only assistant text to stdout and errors to
stderr; JSON/RPC modes write JSONL to stdout with the TUI disabled (`ui_*`
no-ops), so extensions and tests can run headless. `--mode json` emits the
session header first, so JSONL tooling can parse it without special cases.
