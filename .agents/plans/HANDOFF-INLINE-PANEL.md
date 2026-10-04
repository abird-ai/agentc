# HANDOFF — inline-mode resize/ghost work (agentc TUI)

Status of this document: **current as of `master` @ `e8d5431`**, written to let a
fresh agent pick up the inline resize/ghost work with full context and without
re-walking ~20 falsified designs. Read this file first, then follow
"Reading order" below.

Repo: `/home/pvl/spaces/abird/src/agentc` (branch `master`).
Worktree sibling: `/home/pvl/spaces/abird/src/agentc-explore` (branch
`explore-ghost`) — a separate checkout used for the parallel exploration run.

---

## 1. The goal (unchanged since the beginning)

`--tui-mode inline` (the default) must behave like a normal terminal app:

1. **Flow**: content starts right below the shell prompt, grows downward, and the
   composer sits **right after the transcript** (it must *hug* the text, not be
   pinned to the screen bottom with a gap).
2. **Resize must not leave ghosts**: no stale bands, no duplicated composers, no
   stacked chrome, on grow, shrink, width change, or storms.
3. **The terminal scrollback and the shell history are sacred.** Never wiped,
   never cleared, and the app must never push rows into them unless the content
   genuinely cannot fit.
4. **No committed transcript row may ever be lost or duplicated.**
5. **The composer must never enter the scrollback.**
6. **Exit must be clean on every path** (normal, error, signal): the owned panel
   is cleared so the shell resumes on a fresh line.
7. Memory stays O(window); freestanding C23 rules hold (see §9).

Known-acceptable trade-offs already made (do not silently "fix" these):

- When the conversation is shorter than the screen, the composer may sit at the
  bottom with empty space above it (the flowing build) — that is the safe state.
- On terminals whose resize-path cursor report is silent, the app chooses
  under-clear over any placement that could lose a row.

---

## 2. Current state (exact)

| item | value |
| --- | --- |
| `master` | `e8d5431` (docs-only ADR-28 record); **code byte-identical to the last green state** |
| gates | `env HOME=/tmp/agentc-ci-home make check` → exit 0; `env HOME=/tmp/agentc-ci-home ./tests/e2e.sh` → exit 0, zero FAILs |
| release base | `7ad0dfa` (v0.6.0) |
| uncommitted | `include/agent.h`, `include/agentc_ext.h`, `include/plat.h` have **unrelated P6 event-layer edits** (not part of this work; do not commit them blindly) |

Preserved branches / worktrees:

| ref | head | what it holds |
| --- | --- | --- |
| `master` | `e8d5431` | the safe flowing build + the two real fixes (§6) |
| `panel-wip-s7` | `342cdb2` | the full panel/window line (falsified — see §8) |
| `wip-panel-absolute` | `9bde7ff` | panel + harness DSR + scroll fixes |
| `n-emul` | `ccab863` | the emulator drift modes + the ghost e2e block |
| `explore-ghost` (worktree `../agentc-explore`) | `1965625` | the explorer's E/F/G DSR design + full matrix report |

Durable run memory (read these, they are the long-form record):

- `.agents/runs/inline-panel-council/` — `PLAN.md`, `STATUS.md` (the timeline),
  `decisions.md` (**ADR-1 … ADR-28**, the authoritative decision log),
  `VERIFY.md`, `REPORT.md`, `contracts/` (per-seat work contracts),
  `evidence/` (raw gate output, falsifications, A/B logs; `N5–N42`).
- `.agents/runs/ghost-combine-explore/` — the design matrix, proofs and
  falsifications for every combination attempt (REPORT.md, decisions.md,
  STATE.md, contracts/, evidence/). Produced in the worktree `../agentc-explore`
  and copied into this repo so the record is complete.
- `.agents/runs/inline-resize-design2/` — the original Design-2 attempt
  (DESIGN/PLAN/STATUS/VERIFY + `tmux-storm.py`).
- `.agents/design/40-tui.md` — the TUI architecture doc; its inline row and
  resize section were updated as fixes landed. Keep it in sync.
- `.agents/plans/INLINE-RESIZE-DESIGN.md` — the original design brief (§3 holds
  the "terminal-owned seam" statement that keeps re-appearing).

---

## 3. Reading order for a fresh agent (do this in ~15 minutes)

1. This file.
2. `.agents/runs/inline-panel-council/decisions.md` — **ADR-13 … ADR-28** (the
   recent, still-relevant decisions). Skim ADR-1…12 for the early phase.
3. `.agents/runs/inline-panel-council/REPORT.md` §"Residual risk".
4. `src/tui/tui.c`, in this order:
   `tui_inline_frame` (the flowing frame), `inline_resize_erase` (the resize
   path — **the heart of the problem**), `inline_commit_rows_buf` (the commit
   printing — **the known lingering LF site**), `inline_flush_rows_buf`,
   `inline_reflow_h` / `inline_row_width` / `live_w[]`, `tui_apply_resize`,
   `tui_check_resize`.
5. `src/tui/term.c`: `probe_once` / `term_cursor` (the DSR probe),
   `term_set_exit_erase` + `term_leave` (the exit clear).
6. `tests/pty_screen.py` (`Screen`, `resize`, `bottom_anchor`) and the storm
   block in `tests/tui_e2e.py` (the oracle).
7. §10 (measured real-terminal facts) and §8 (falsified paths) below — read these
   before proposing any design.

---

## 4. Architecture in one screen

Inline rendering is **cursor-relative**: the app paints an owned region
(composer - menu + status, `h = live_rows` rows) at the terminal's cursor and
parks the hidden cursor at the region's top-left. The transcript is printed once
(`inline_commit_rows_buf`) and never redrawn. On resize the app must erase the
old region and repaint it — and that is where everything went wrong, because:

- the terminal reflows on resize and **moves the parked cursor**, so a
  cursor-relative erase starts at the wrong row (→ stale band / stacked composer);
- the app's **own region rows reflow too** (full-width rules re-wrap on
  narrowing), so `live_rows` is no longer the on-screen height;
- the app **cannot see** where the terminal put anything after a reflow.

The resize path therefore tries to recover a *coordinate* (the DSR cursor report)
and place the erase/repaint conservatively (`class=E/W/R/F/G` dispatch inside
`inline_resize_erase`, with the rule: **under-clear is acceptable; a lost row is
never acceptable**).

Key state on `Tui` (see the struct in `src/tui/tui.c`): `live_rows`,
`live_cursor_off`, `live_w[]`, `park_row`, `park_known`, `probe_row`,
`committed_rows`, `committed_blocks`, `committed_partial`, `dsr_dead`-era flags
(removed), `inline_panel_top` (panel line only, not on master).

---

## 5. Chronology — what was tried, in order (all commits are on the preserved refs)

### Phase 1 — Design 2 (screen-owned window)

`fcddc84` bounded window → `0ce294a` running window → `5331984` `ESC[2J` on
resize → **revert `f8f6d21`** back to the flowing renderer.
Outcome: owning the screen changed the look (bottom-pinned, blank above) and
cleared visible history; rejected by the user.

### Phase 2 — DSR experiments

`d6f065e` absolute-row anchor via DSR → `f1b3f0b` calibration → **revert
`8b98ad0`** → `b5a7e98` calibration that wiped the screen → **revert `543b9a1`**.
Outcome: the terminal's DSR is unreliable/ambiguous; calibration is dangerous.

### Phase 3 — takeover, then un-takeover

`9b12366` flow until first resize → `f5b4256` top-anchored window → `e80aaae`
no flush on resize → `dd72be7` bottom-anchored window → **`7bb9e50` revert the
takeover entirely**; `2136caf` freezes the scrollback contract in tests.
Outcome: the takeover is rejected; the scrollback contract test is born.

### Phase 4 — the contract test exposes a gap

`2136caf` asserted no `ESC[2J`/`ESC[3J` and a byte-identical app mirror, but
**not** that the terminal's scrollback survives — a gap later exploited by
`ESC[S`.

### Phase 5 — council round 1 (the panel line), `8d0fe72` … `342cdb2`

`8d0fe72` own the panel to the bottom → `bb845e5`/`42d3bd4`/`0f47c6b` harness
DSR + scroll fixes + bottom-anchored emulator → `23234c1` fixed-top panel →
`cf02810` ADR-7 app-causation guard → `71be78b`/`39dee27`/`9bde7ff` fixes →
`c85c2bf` … then the widening duplication → S7 moving-top fix `ddad69d` (verified
good for pure widen) → **S4f critic falsified V2 (height change after a reclaim
strands a row) and V6 (widen reclaim after block drops overwrites a row)** →
S8 corrective rounds failed → **`778775b` reverted master to `2136caf`**, panel
line preserved on `panel-wip-s7` (ADR-9). ADR-10/11/12 = the ghost hunt begins.

### Phase 6 — targeted fixes on the safe base (current line)

| commit | what |
| --- | --- |
| `768f1e1` | reproduce xterm.js resize cursor drift in the harness (additive emulator modes + ghost e2e block) |
| `ba1a28b` | 6a clamp-normalize on a height grow (safe: identity on a no-drift terminal) |
| `b8de02c` | explorer's E/F/G DSR re-anchor ported (reply-aware classes) |
| `9ee073d` | single-query probe; remove the sticky `dsr_dead` (98/98 resizes had been class G) |
| `7cd226a` | reply-aware width-change class W |
| `05c4df1` → **`5620493` revert** | one-shot shrink takeover via `2J` + window reprint — **falsified** (N11: exactly-once seam duplication, scrollback growth, narrow-width loss) |
| `ed32b19`, `920a11e`, `202df16` | region-only re-anchor on width shrink + guards |
| `585db49` | DSR deadline 60 ms → **300 ms** (the known-good value; 60 ms lost every resize reply) |
| `13709a9` | restore the 6a probe semantics (the pre-drain had been **discarding the in-flight reply**) |
| `f321f9c` → **`ee494e0` revert** | absolute bottom-anchored erase — **falsified** (N23: committed-row loss on a top-anchored no-reply terminal) |
| `b35a8bd`, `6cd29ff` | default **no-reply absolute region reposition** + kill-switch (region-only; never touches the transcript) |
| `2f08560` | **absolute per-row resize paint** (no LF walk) |
| `8db56e6` | **LF-free commit-bearing resize frames** — fixes "the screen eats a row per resize" |
| `0083388`, `7d6a6bd` | no-reply width grows covered + the top-anchored width-grow law in the e2e |
| `e8d5431` | docs: **ADR-28** — the pending design (below) |

---

## 6. What is genuinely fixed and verified on `master`

1. **The app's own resize scroll is gone.** Resize frames no longer emit a
   newline at/below the bottom row; `APP_SCROLL_EVENTS=0` and scrollback
   byte-identity (`IDENT=1`) in the verified scenarios. This was the "main screen
   eats a row on every resize even when there is space" defect (seen in tmux).
2. **The no-reply width-grow artifact fix** (`0083388`): a widening no longer
   leaves the previous region's old-width rows behind.
3. **The reply-aware classes actually engage when the terminal answers**
   (was 0/13, 0/16 in real runs; now partial, e.g. 6/30).
4. **The scrollback contract is frozen in tests** (`test_inline_resize_preserves_scrollback`)
   plus the e2e marker assertions, the app-causation guard, and the new
   no-scroll / byte-identity invariants.

---

## 7. The one open defect and the accepted design to fix it (ADR-28)

**Defect.** On a **silent resize path** (probe does not answer — this is the
caller's real terminal), a shrinking/widening resize can push rows into the
scrollback **even when the reflowed content would fit**, because the commit
printer (`inline_commit_rows_buf`, newline mode) is reached and its trailing LF
scrolls at the bottom row.

**Why it is hard (proved — do not re-derive).** After the terminal
bottom-flushes the region, the rows above it are *either* free blank space (a
commit should fill them — no scroll) *or* committed content (a commit must push).
A single cursor coordinate cannot distinguish them, so:

- LF-free absolute placement ⇒ overwrites committed rows (**loss**),
- cursor-relative placement ⇒ **scrolls when it fits**.
"Never scroll when it fits" and "never lose a row" are **mutually unsatisfiable
without a discriminator**. Both candidate fixes and the deferred post-reflow
probe were implemented and falsified (`evidence/N40-blocker.txt`, ADR-26/27).

**The discriminator is computable from our own output** — the app prints every
committed row, so it knows each row's width exactly as it already knows
`live_w[]` for its own region. ADR-28 design (ready to implement):

1. **Record** each committed row's content width as it is emitted, using the
   same `inline_row_width()` measurement as `live_w[]`, in a **ring of capacity
   `GRID_MAX_ROWS`** indexed by absolute committed-row number.
   *Memory proof*: if `committed_rows <= rows <= GRID_MAX_ROWS` the ring is
   exact; if `committed_rows > GRID_MAX_ROWS` then the reflowed height
   ≥ `committed_rows > rows`, so the screen is full and the gap is 0 regardless
   of overwritten slots. O(`GRID_MAX_ROWS`) ints, no per-session growth.
2. **Compute** the post-reflow content end: the flowing law puts the region
   directly below the transcript, so the pre-resize content end is
   `park_row - 1`, and
   `content_end = park_row - 1 + (Σ ceil(w_i / c1) - committed_rows)`
   (clamped to the screen; if the sum exceeds `rows - h` the screen is full).
3. **Place**, with the bottom-flush region top `top = rows - h + 1` and
   `gap = top - 1 - content_end`:
   - `gap >= ncommit`: paint the commits **absolutely** at
     `top - ncommit … top - 1`, region at `top` — **no scroll, no overwrite**;
   - `gap < ncommit`: fill the gap, push **exactly** `ncommit - gap` (genuine
     overflow), paint the rest bottom-flush;
   - never an LF at/below the bottom row; `park_known == false` keeps today's
     safe repair unchanged.

**Where to implement**: `inline_commit_rows_buf` / the absolute variant used on
the resize path, plus the no-reply branch of `inline_resize_erase`. Then:

**Regressions to add** (must be in the gates):

- the N40 standalone case — a short transcript that **fits** after a no-reply
  width shrink, then a streaming answer: `APP_SCROLL_EVENTS` must go **10 → 0**
  with the shell marker intact on screen **and** in the scrollback, under both
  emulator models;
- width-record exactness after widening, narrowing, height grow/shrink,
  alternating storms, `/new` session swaps, and mid-frame resizes;
- keep every existing invariant (see §11).

**Open assumption to test**: the shell prelude above the app's content is not in
the records; the design assumes hard rows for app content and a non-growing
prelude. The regression set above is what tests it.

---

## 8. Falsified paths — DO NOT RE-WALK (raw evidence in the run dirs)

| attempt | why it failed |
| --- | --- |
| screen takeover / owned window (top or bottom anchored) | duplicates or loses rows; clears visible history; user rejected the look |
| `ESC[S` (SU) for scrolling | **xterm.js discards** the scrolled lines → ate the shell history |
| `ESC[2J` + window reprint on shrink (`05c4df1`) | exactly-once seam duplication, scrollback growth, narrow-width loss (N10/N11) |
| absolute rule `region_top = min(r_after, rows-h+1)` (`f321f9c`) | eats a committed row in the top-anchored model (N23) |
| deferred post-reflow DSR probe (ADR-27) | a post-reflow *cursor* row is not the free-gap extent; measured no change (`N40`) |
| 60 ms DSR deadline | every resize reply lost during a storm (real terminal) |
| pre-drain "stale reply" discard in `probe_once` | discarded the **current** query's reply → 0/13 replies |
| sticky `dsr_dead` | one timeout disabled probing for the whole process → 98/98 class G |
| fixed-top / moving-top panel window | V2/V6 exactly-once seams across drops/height changes/reclaims (ADR-9) |
| erase-union / idempotent overwrite / DSR-absolute variants | break the frozen `ESC[J` assertions or re-emit committed rows (ADR-10) |
| full-width region rows | re-wrap on narrowing → the region's on-screen height ≠ `live_rows` |

---

## 9. Hard constraints (violating these fails review)

- Freestanding C23: only `<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>`; use
  `agentc_alloc/realloc/free`; no stdio/stderr except `agentc_out*`/`agentc_log*`;
  one module per file.
- **Do not modify anything under `include/`** (public headers are append-only).
  (`include/` currently has unrelated uncommitted P6 edits — leave them alone.)
- **Never emit `ESC[3J`** (never wipe the scrollback). **Never emit `ESC[2J`**
  (no whole-screen clear). **Never emit `ESC[S`** (discards in xterm.js).
- Keep `--tui-mode scrollback` and `fullscreen` behaviour unchanged.
- **Do not weaken or delete tests.** Update an expectation only when the
  trade-off is explicit, labelled, and documented in the run dir.
- A terminal that does not answer the cursor report must keep a safe fallback.
- Type-ahead must never be lost, and a DSR reply must never be fed to the input
  path as keystrokes.

---

## 10. Measured real-terminal facts (VS Code / xterm.js) — the ground truth

From the caller's runs (`AGENTC_TUI_WRITE_LOG`):

- The **startup** DSR answers (`@ dsr start r=8/17/18`).
- The **resize-path** DSR usually does **not** answer: 0/13, 16/16 runs; a later
  run answered 6/30. Never rely on it.
- The region is **bottom-flushed after a resize**; the cursor lands at the region
  top (`rows - h + 1`) or the region bottom (`rows`). All 51 replies with
  `rows=34, h=4` were **31 or 34** — zero exceptions.
- The app's own **region rows are reflow-unstable** (full-width rules re-wrap on
  narrowing).
- The **tmux repro** (`W \r\n` as the last write) proved the app was scrolling
  the screen itself — that class of bug is fully testable in-harness, unlike
  reflow.

---

## 11. Test infrastructure and how to reproduce locally

- **Unit/golden**: `tests/tui_test.c` with `src/tui/tui_test.h`; golden output
  `tests/data/tui_test.expected` (regenerate after intentional changes:
  `./build/test/tui_test > tests/data/tui_test.expected`). Test hooks relevant
  here: `agentc_tui_test_resize`, `agentc_tui_test_resize_mid_frame`,
  `agentc_tui_test_panel_seed`, `agentc_tui_test_screen/scrollback`.
- **E2E (the oracle)**: `tests/e2e.sh` → `tests/tui_e2e.py` driving a real pty
  against `tests/pty_screen.py`. The emulator models, all additive, never
  replacing each other:
  - default `Screen` = tmux-style (keeps the cursor visible),
  - `Screen(..., bottom_anchor=True)` = xterm.js-style (content anchored to the
    bottom),
  - the `n-emul` drift modes (`cursor_drift=<rows>`, `cursor_to_bottom`,
    `bottom_grow`) and the ghost scenario blocks (`control`, `bottom_grow`,
    `mid_screen`).
  The harness answers `ESC[6n` with `DSR_REPLY_ROW` so the reply paths execute.
- **Invariants asserted today**: hint exactly once; exactly 2 composer rule
  rows; no rule rows in the scrollback; shell marker exactly once; committed
  lines exactly once and in order; `test_inline_resize_preserves_scrollback`
  (no `2J`/`3J`, no re-emit, byte-identical app mirror); `APP_SCROLL_EVENTS=0`;
  scrollback byte-identity (`IDENT=1`); the region reflow-stability invariant;
  the raw-stream `2J/3J` guard.
- **Real-terminal instrumentation**: `AGENTC_TUI_WRITE_LOG=/tmp/agentc-tui.log`
  logs every write (`W <escaped bytes>`), notes (`@ frame`, `@ resize …`),
  the resize classes (`@ dsr resize class=E/W/R/F/G r=… park=… h=… geom=…`) and
  the drift probe (`@ probe resize … r_before=… r_after=… drift=…`).
- **tmux recipes** (fast, deterministic, and the only way to catch app-caused
  scrolling):

  ```text
  tmux new-session -d -s t -x 80 -y 24 "echo H1; echo H2; exec ./build/agentc --api-key t --offline --no-session; sleep 30"
  tmux set-option -t t window-size manual
  # drive /help, then resize repeatedly:
  tmux resize-window -t t -x 100 -y 34 ; tmux resize-window -t t -x 60 -y 18
  tmux capture-pane -p -S - -t t          # screen + scrollback
  ```

  Assert: composer rule rows on screen == 2; the scrollback contains no rule
  rows; the shell markers appear exactly once; the scrollback length does not
  grow across resizes when the content fits.
- Mock provider: `tests/mock_provider.py` (used by the e2e turn/stream storms).

---

## 12. Process conventions (how the council ran, and how to run it again)

- Durable memory per mission: `.agents/runs/<slug>/` with `PLAN.md`,
  `STATUS.md` (timeline + next action), `decisions.md` (**append-only ADRs**),
  `VERIFY.md` (every command + raw evidence path), `contracts/` (per-seat work
  orders), `evidence/` (raw outputs, named `N<round>-*`).
- Every landing requires: both gates green, a pre-fix-FAIL → post-fix-PASS A/B
  for the specific defect, an adversarial critic pass, no invariant weakened,
  and an ADR recording the change **and its residual risk**.
- Falsifications are first-class results — record them with raw evidence so
  nobody re-walks them (§8 exists for that reason).
- When a mission cannot be made clean, apply the fallback: **leave `master`
  byte-identical to the last green state**, preserve the work on a branch, and
  write the falsification into the ADRs.

---

## 13. First actions for the next agent

1. Run the gates to confirm the baseline is green:
   `env HOME=/tmp/agentc-ci-home make check` and
   `env HOME=/tmp/agentc-ci-home ./tests/e2e.sh`.
2. Read §3, then §7 (the ADR-28 design), then §8 (do not re-walk).
3. Implement ADR-28 steps 1–3 (the ring of committed-row widths, the
   `content_end` computation, the gap-based placement), starting in
   `inline_commit_rows_buf` + the no-reply branch of `inline_resize_erase`.
4. Add the N40 regression (fits-after-shrink: `APP_SCROLL_EVENTS` 10 → 0, marker
   intact on screen and in the scrollback) and the width-record exactness cases
   from §7.
5. Verify, add the ADR, land only if both gates are green; otherwise record the
   falsification and leave `master` safe.
6. Keep `.agents/design/40-tui.md` and the run-dir docs in sync.
