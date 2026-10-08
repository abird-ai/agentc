# Inline-mode resize: problem, investigation, and Design 2 (chosen)

Status: **open / not implemented.** The tree is intentionally at the last known-good
state (`8c83a1b`) after three attempted fixes were reverted. This document is the spec
for landing the real fix. Read `HANDOFF.md` first.

---

## 1. Problem statement

`--tui-mode inline` (the default) owns a rectangular region at the bottom of the
terminal and repaints it every frame. On some terminals, **resizing the window leaves
one ghost copy of the composer (top rule + hint + bottom rule) in scrollback per
resize**. The reporter's sequence: **grow the window a few times, then shrink** — the
ghosts accumulate and the terminal scrollbar grows.

The reporter's requirement, in their words:
> inline mode resizing shouldn't be affecting the screen like this… we own all the
> bytes, and resize, we cleanly redraw everything from the app to the new screen. Our
> actual screen itself should have never exceeded the current height; it's always
> capped at the current terminal height and we'll redraw our text fully inside as
> scrollable text with a scrollbar when it exceeds.

The reporter also wants to **avoid keeping the whole transcript in memory** (Design 1
below owns the transcript; they asked for a Design 2 that does not).

---

## 2. Reproduction

- **tmux (a real reflowing terminal): clean.** Script:
  resize a detached tmux pane through many sizes (including grow-then-shrink) and
  `capture-pane -p -S -` — `Ready. Press` appears exactly once. tmux keeps the cursor
  on the same logical line across reflow, so the parked-cursor assumption holds there.
- **`tests/pty_screen.py` (the CI terminal emulator): clean.** The storm test
  (`tests/tui_e2e.py`) passes.
- **The reporter's terminal: broken.** It evidently repositions the cursor (or reflows
  such that the parked cursor lands on a different row), so the erase misses and the
  region is printed off the bottom, scrolling a ghost into scrollback per resize.

Because neither tmux nor the emulator reproduces it, the fix must be **terminal-
independent** — it must not depend on where the terminal leaves the cursor.

---

## 3. Root cause

Inline mode parks the hidden hardware cursor at the region's **top-left** after each
frame (`\x1b[<h-1>A\x1b[1G`). Every frame and every resize erase is cursor-relative:
the resize path (`tui_apply_resize`) calls `inline_erase_owned()`, which assumes the
cursor is still at the region top and clears downward (`\r\x1b[J`).

A terminal resize reflows lines and may move the cursor. When the parked cursor is no
longer at the region top, the erase clears the wrong rows, the next frame prints the
region at a stale position, the terminal scrolls to make room, and a ghost copy is
left behind. Repeat per resize.

A secondary problem: the **seam** between the app's on-screen content and the
terminal's scrollback is owned by the terminal (it decides how much reflows and what
scrolls off), so any "reprint the on-screen part" scheme can duplicate or drop rows at
that seam. This is why the fix needs a design where the screen and the scrollback are
kept disjoint by construction.

---

## 4. What was tried (all reverted; keep for the record)

1. **`\x1b[J` erase from the parked cursor** (committed `272a905`). Storm-safe (tmux/CI
   green) but does not fix terminals that move the cursor — the reporter still saw
   ghosts. This is the current HEAD behaviour.
2. **Clear the whole screen and reprint the transcript from memory.** Fixed the screen
   but **duplicated** history (the old copy is still in the terminal scrollback) — the
   storm guard reported a marker 5×. Reverted.
3. **Bottom-anchor the region and repaint with absolute moves every frame.** Removes
   the parked-cursor dependence, but it changes where committed content lives, breaking
   the storm/golden model (composer rules duplicated, committed markers lost). Reverted.

Lesson: you cannot both (a) let the terminal own the scrollback *and* (b) reprint
on-screen content on resize, unless the screen and scrollback are disjoint by
construction.

---

## 5. Design 1 — owned viewport (reference; rejected for memory)

**Idea.** The app owns the whole viewport and the transcript; it renders a window of
the in-memory transcript and never commits rows to the terminal mid-session.

- `chat` (the whole transcript) is the source of truth.
- Frame: render a window `[T - tv - scroll, T)` (sticky bottom + `chat.scroll`) into
  the region, capped to the terminal height, with the scrollbar.
- Resize: `\x1b[2J` + repaint the window. Deterministic, no cursor trust.
- Exit: dump the transcript to the terminal scrollback once.
- Trade-offs: transcript only in the terminal scrollback after exit; shell history
  above overwritten on screen while running; memory O(whole transcript) — **the reason
  the reporter rejected it.**

---

## 6. Design 2 — screen-owned bounded window + flush-to-scrollback (CHOSEN)

**Idea.** Keep the screen and the terminal scrollback **disjoint by construction**:
the screen only ever shows a *bounded in-memory window* (recent rows); everything older
has been handed to the terminal and is gone from memory. Memory is O(window), not
O(transcript), and resize is a deterministic clear + repaint.

### 6.1 Invariants

1. **Screen ∩ scrollback = ∅.** The screen holds only the window (transcript tail +
   queue + composer + menu + footer). Committed rows are flushed to the terminal
   scrollback and never remain on screen.
2. **The cursor is parked where a height-shrink cannot scroll it** (e.g. row 1). This is
   load-bearing: if a resize scrolls the screen, rows are pushed into scrollback and the
   seam drifts.
3. **Only the app scrolls the terminal**, and only to flush committed rows (batched).
   The scrollback therefore contains exactly the rows the app flushed, in order.
4. **Resize = `\x1b[2J` + repaint the window.** The screen holds nothing that is in
   scrollback, so reprinting is exact: no duplicates, no drops.

### 6.2 Memory model

- Keep the last `K` transcript rows (window), `K` ≈ a few screenfuls plus the composer.
  `chat` blocks entirely older than the window are freed after their rows are flushed.
- Nothing else grows with session length.

### 6.3 Mechanism (per frame, inline)

Let `rows` = terminal height, `win` = window rows kept in memory, `C` = rows already
flushed to scrollback (tracked).

1. Compose the window grid: the last `rows` rows of output = `[transcript tail]`,
   `[queue]`, `[composer]`, `[menu]`, `[footer]`. The transcript tail is the last
   `win` in-memory rows.
2. Flush any newly-finished rows that have left the window: print them at the top of
   the screen and scroll them off (batched, respecting the window/blank budget), which
   appends them to the terminal scrollback. Advance `C`.
3. Position the region with an absolute move and repaint in one write (no parked
   cursor needed; the cursor is parked at row 1 between frames).
4. On resize: `\x1b[2J`, recompute the window height from the new `rows`, repaint.
5. On exit: nothing special beyond the current shutdown (the flushed history is already
   in scrollback; print the still-uncommitted tail).

### 6.4 Why the seam is clean

The terminal's scrollback grows only when the app emits the explicit flush scroll, and
the cursor never sits where a resize would add an extra scroll. So scrollback =
`[0, C)` and screen = `[C, C+window)`, contiguous, on every terminal.

### 6.5 Trade-offs

- The app owns the screen while running: shell history above the region is overwritten
  on screen (`--tui-mode scrollback` remains the append-only mode). This is the same
  screen-ownership trade-off as Design 1.
- The flush path is intricate: printing committed rows at the top and scrolling, then
  repainting the window, needs care when many rows flush at once (do not overwrite the
  window before the scroll; batch in bounded chunks).

### 6.6 Edge cases to handle

- **Very small terminals** (`rows` < composer+footer): the budget must clamp, as today
  (`TuiFrameBudget`); window = 0.
- **A block taller than the window** (a huge tool result or paste): render its tail
  (sticky bottom) and flush the older rows of the same block in bounded chunks; do not
  try to hold the whole block.
- **Mid-frame resize**: keep the existing discard-and-reapply path (`resize_pending` /
  `geom_gen`), but the reapply is now "clear + repaint", not "erase from the cursor".
- **Transcript cleared / session swap** (`/clear`, `/new`, `/resume`): reset `C` and the
  window, `\x1b[2J`, repaint; committed history stays in scrollback.
- **Scrollback-mode parity**: leave `--tui-mode scrollback` as-is (append-only, no
  owned region).

---

## 7. Implementation plan (Design 2)

Files: primarily `src/tui/tui.c` (`tui_inline_frame`, `tui_apply_resize`,
`tui_inline_shutdown`), possibly `src/tui/components.c` (scrollbar already exists), and
tests. Public headers unchanged (append-only if needed).

Suggested state on `Tui`:
- `size_t flushed_rows;` — rows already handed to the terminal scrollback.
- `size_t window_rows;` — in-memory window height (rows).
- Reuse `chat.scroll` for the in-window scroll; `commit`/`committed_*` may be repurposed
  or retired for inline (scrollback mode keeps its own path).
- `bool reanchor;` for the first write after a resize (not strictly needed once the
  region is always written with an absolute move).

Algorithm sketch for `tui_inline_frame`:
1. `h` = region height (composer+footer+menu+queue + min(transcript_tail, rows-…)).
2. `top` = `rows - h + 1` (bottom-anchored) or `1` (top-anchored) — pick one consistently
   and park the cursor accordingly; bottom-anchored with the cursor parked at row 1
   satisfies invariant 2.
3. Compute the window top row `C = total - h_transcript`; if `C > flushed_rows`, flush
   `C - flushed_rows` rows at the top (batched), then `flushed_rows = C`.
4. `\x1b[<top>;1H` + render the window; park the cursor at row 1 (hidden).
5. On resize (`tui_apply_resize`): `\x1b[2J`, clamp geometry, `reanchor = true`.
6. Free `chat` blocks whose rows are entirely below `flushed_rows`.

**Important:** do not call `inline_erase_owned()` from the resize path (it is
cursor-relative); the resize path should clear the screen. `inline_erase_owned[_buf]`
may become removable. Keep the "screen must never exceed the terminal height" property
(the region is clamped by `TuiFrameBudget`).

`tui_inline_shutdown`: the flushed history is already in scrollback; print the
still-uncommitted tail (from the window) and a newline.

---

## 8. Test migration plan

Tests/goldens that encode the old "commit to scrollback" inline model (expected to need
rewriting around "window on screen, flushed rows in scrollback exactly once"):

- `tests/tui_test.c`, `test_inline_owned`: `inline_commits_user`,
  `inline_commits_answer`, `inline_commits_tool`, `inline_commit_once`,
  `inline_no_duplicate_commit`, `owned_commit_once`, `owned_commit_flows_down`,
  `owned_commit_relative`, `owned_relative_repaint`, `owned_cursor_parked`,
  `owned_resize_erase_reflow`, `owned_midframe_erases`, `owned_resize_commit_once`,
  `owned_resize_no_stale_hint`, `owned_resize_reanchor`.
- Cursor-agreement tests: `inline_cursor_hardware_agrees`,
  `inline_cursor_midline_hardware_agrees` (the cursor is now parked deterministically,
  so assert against `agentc_tui_test_region_top(t)` + the caret).
- `tests/tui_test.c` inline scroll/paging tests (`test_inline_paging`,
  `test_scroll_anchor`, `test_large_commit`) — re-check which now scroll the window vs
  the terminal.
- `tests/tui_e2e.py` storm: `composer rules` (expect 2), `committed line count`
  (markers once), `shell history marker lost/duplicated`. Rewrite for the new model:
  the transcript window may be on screen; flushed rows appear once in the emulator's
  scrollback.
- `tests/pty_screen.py` already understands `ESC[J`; may need `ESC[2J` (it likely does;
  verify) and the flush path.
- Regenerate `tests/data/tui_test.expected` after the label set changes.

Add a regression test that grows then shrinks and asserts the composer rules appear
exactly twice on screen (and no ghost rows), driven by the harness; and a tmux script
(see §2) as the manual/real-terminal check.

---

## 9. Open questions / risks

- Is the screen-bottom or screen-top anchoring preferred for the window? Bottom keeps
  the familiar "region at the bottom" look with blank above; top makes the seam
  trivially stable but changes the look. The invariant only needs the cursor parked
  where a shrink cannot scroll it.
- How aggressively to flush (per row vs batched per frame) for smoothness; avoid a
  full-screen scroll per row.
- Whether `chat.scroll` should scroll the window or the whole transcript; with Design 2
  the window is small, so PgUp/PgDn likely page within the window and rely on the
  terminal for older history.
- Confirm the terminal emulator's `ESC[2J` modelling matches the real one used by the
  reporter.

---

## 10. References

- Master context: `.agents/plans/HANDOFF.md`.
- Current inline implementation: `src/tui/tui.c` (`tui_inline_frame`, `tui_apply_resize`,
  `tui_inline_shutdown`, `TuiFrameBudget`), design notes `.agents/design/40-tui.md` §1
  and §1.4.
- Scrollbar: `comp_scrollbar` in `src/tui/components.c`.
- Test emulator: `tests/pty_screen.py`; storm harness: `tests/tui_e2e.py`.
- Run record for the (reverted) attempts is folded into §4 above; the per-task run dirs
  are `.agents/runs/*`.
