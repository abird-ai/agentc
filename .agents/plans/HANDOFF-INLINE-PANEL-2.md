# HANDOFF — inline resize ghosts / blank-lines (agentc TUI), round 2

Status as of `master @ e7f8d39` (N51). Supersedes the resize sections of
`.agents/plans/HANDOFF-INLINE-PANEL.md` (round-1). Read this file first, then the
run dir. Repo `/home/pvl/spaces/abird/src/agentc`, branch `master`.

The durable memory is `.agents/runs/inline-panel-council/`:

- `decisions.md` — ADR-1 … ADR-35 (authoritative; read ADR-30 … ADR-35).
- `evidence/N44-*` … `N51-*` — raw gate output, measurements, falsifications.
- `design/N44-*`, `design/N45-BRIEF.md` — design docs.
- `STATUS.md` — timeline + the ops rule (bounded commands).

---

## 1. The mission (unchanged)

`--tui-mode inline` must behave like a normal terminal app. The one remaining
defect: **on a resize the app still (a) occasionally leaves a ghost band and
(b) inserts blank rows that scroll the content up.** Grow and shrink both.

Hard requirements still in force: never `ESC[2J` / `ESC[3J` / `ESC[S`; no
committed row lost or duplicated (exactly-once); the composer must never enter
the scrollback; the shell prelude/history is sacred; freestanding C23; **do not
modify anything under `include/`** (it has unrelated uncommitted P6 edits — leave
them, and leave the untracked `src/core/actor*.c`, `src/core/process.c`,
`tests/actors_test.c`, `tests/data/actors_test.expected` alone);
`--tui-mode scrollback` and `fullscreen` unchanged; type-ahead never lost; a DSR
reply never fed to input as keys.

## 2. Current state (exact)

| item | value |
| --- | --- |
| `master` | `e7f8d39` (N51: adaptive resize-probe skip + grow-band erase) |
| code before it | `d4d3016` (N50 falsification docs), `cb18823` (N49 silent anchor), `fedde27` (N46 content anchor) |
| gates | `env HOME=/tmp/agentc-ci-home make check` → EXIT 0; `... ./tests/e2e.sh` → EXIT 0 zero FAIL; `AGENTC_GHOST_REPRO=1 tui_test` → zero `=0` |
| uncommitted | unrelated P6 edits under `include/`, `src/base/`, `src/core/`, `src/plat/`, `tests/tools*` (do not commit or revert) |

**In-flight when this was written:** a subagent may be running with a spec
`origin = ce+1` (session 01a1299e). That spec is likely WRONG (see §4). Stop it
or verify before building on it.

## 3. Architecture in one screen

Inline renders a cursor-relative owned region (`live_rows` rows: transcript
tail - queue + composer + menu + footer) at the terminal cursor and parks the
hidden cursor at the region top-left. The transcript is printed once
(`inline_commit_rows_buf`) and never redrawn. On resize the terminal reflows and
moves the parked cursor; the app cannot see where anything landed.

Key `Tui` state (`src/tui/tui.c`): `live_rows`, `live_w[]`, `commit_w[]` (per
EMITTED committed row width), `committed_rows/blocks/partial`, `park_row`/
`park_known`, `pinned` (region reached the bottom), `has_history`,
`content_top`/`anchor_ok`/`anchor_total`/`anchor_rows`/`anchor_w` (N46/47
content-anchor), `resize_probe_miss` (N51).

Key functions: `tui_inline_frame` (the flowing frame; sets `pinned`), `inline_resize_erase`
(the resize path — THE BUG LIVES HERE: content-anchor block + ADR-30 flush +
reply-aware E/W/R + pinned absolute erase), `inline_commit_rows_buf` / `_at`,
`inline_reflow_h` / `inline_row_min_cells` / `inline_row_width`,
`inline_anchor_total_at` (N47), `committed_rows_at`, `tui_apply_resize`,
`tui_check_resize`, `tui_probe_row`.

## 4. The current defect, with exact evidence

The caller's terminal is VS Code (xterm.js). From the real log
`/tmp/agentc-tui.log` (N51 build), 88 `content-anchor` events, all `h=4`:

```text
origin < ce+1 (erase starts INSIDE content): 74/88
origin == ce+1:                              14/88
typical: (100,34)->(86,34) origin=28 top=31 ce=30 full=30 h=4   (H_stale=7)
```

- `H_stale = Σ ceil(live_w[i]/c1) = 7`: the old-width `cols−1` composer rows
  re-wrap to ~2 rows each on a narrow shrink.
- `origin = min(ce+1, rows − H_stale + 1)` → `min(31, 28) = 28`.
- `full=30`, `rows=34`, `H_stale=7` ⇒ `30 + 7 = 37 > 34`, i.e. **3 rows have
  scrolled**, so the on-screen content end should be `34 − 7 = 27`, but the app
  reports `ce = 30` (it does NOT subtract the scroll). The app cannot observe
  terminal-side scroll (the N43 wall).

Two symptoms follow from that `ce` vs on-screen mismatch:

- **blank rows / content moving up** when `origin` (28) is treated as reaching
  into content rows 28–30;
- **ghost band** when the true band top is above `28` and the erase misses it.

Two readings the log alone cannot separate (read the `ce` computation in HEAD to
decide): (A) `ce=30` is the on-screen end and `origin=28` blanks content; (B)
`ce=30` is the pre-scroll height and `origin=28 = rows−H_stale+1` is the true
band top (then the blank/ghost has another cause).

**RESOLVED by `evidence/N52-xterm-band-reflow.txt`** (real `@xterm/headless`
measurement, added after this doc's first draft): the re-wrapped composer band
**always extends DOWNWARD from the on-screen `ce+1`** — fitting (baseY=0,
firstRule=`ce+1`), overflow (scrolled, firstRule=`ce+1`), and the caller's exact
122×49→118×49 geometry (content end screen row 44, firstRule row 45 = `ce+1`,
gap 0). So:

- the erase origin is simply **`ce_on_screen + 1`**; the band grows down and
  `ESC[J` from there covers it; the `min(·, rows−H_stale+1)` term is unnecessary;
- the bug is that `ce` was **pre-scroll** (`full=30 + H_stale=7 > rows=34` ⇒ 3
  rows scrolled ⇒ true on-screen end 27), so `min` picked 28 instead of the true
  band top — the two agree only once `ce` is the on-screen end;
- the harness's "band above `ce+1`" (N50) is a `tests/pty_screen.py` hard-row
  re-wrap artefact, not the real engine.

So the fix (reading A, corrected): **make `ce` the on-screen content end**
(`ce_on = (content_top−1 + anchor_total) − max(0, (content_top−1 + anchor_total) +
H_stale − rows)`), then `origin = ce_on + 1`; drop the `min`. This is consistent
with both the fitting and the overflow real-engine cases.

The first resize after startup is the most likely to ghost (fresh reflow with no
prior `park`/anchor state).

## 5. What is genuinely fixed / verified

1. **Faithful reflow model** (`tests/pty_screen.py`): `bottom_anchor` trims blank
   pad rows; the unfaithful `width_bottom_flush` down-shift is removed. Measured
   xterm.js/tmux behavior: TOP-ANCHORED on a width shrink (content at row 1,
   blanks below, overflow pushes top rows to history). `N44-measure-reflow.txt`.
2. **Shrink-scroll reset + flow-then-pin** (ADR-30/31): the region flows with the
   content while short, pins at the bottom once the transcript fills; `pinned`
   includes the shell prelude.
3. **Silent-path content anchor** (N46/N49): `content_top` captured at startup;
   `anchor_total` = reflowed height of banner + emitted committed rows; guard
   `content_top>=1 && anchor_total>0 && ce>=1`; the ADR-30 flush rebases the anchor.
4. **Adaptive resize-probe skip + grow band** (N51): the per-resize DSR probe
   blocks only until its first miss (300 ms stall measured: mean 0.278 s → 0.036 s,
   DSR queries 9 → 1); a grow clamps the erase origin to the old-geometry top.
5. **Corpus + test blueprint**: `N47-log-corpus.md`, `N48-corpus-matrix.md`
   (real vs harness attribution; T1–T12 replay matrix).

**The real terminal is SILENT on resize** (N48: the real log has ZERO `dsr resize`
events; `r=24/34/28/1` patterns are harness spoofs). Treat all non-zero replies as
MODELLED, not measured.

## 6. Falsified paths — DO NOT re-walk

- Screen takeover / owned window (top or bottom anchored); `ESC[S`;
  `ESC[2J` + window reprint (N10/N11 duplication); absolute
  `region_top=min(r_after, rows-h+1)` (N23 loss); deferred post-reflow probe
  (ADR-27); 60 ms DSR deadline; pre-drain reply discard; sticky `dsr_dead`.
- ADR-28 per-committed-row ring as the reflow GAP discriminator (N43) — the ring
  is fine as a committed-reflow sum, NOT as a top-anchor/bottom-flush
  discriminator.
- ADR-34/N50: dropping the `min(ce+1, rows−H_stale+1)` term (leaves ghost rows in
  the harness) and an UNCONDITIONAL probe skip (turn-storm `done=0` loss). The
  `min` is load-bearing in the harness; whether it is right on the real terminal
  is exactly the open question in §4.
- `N47` reply/park patterns as "real terminal behavior" (misattributed; see N48).

## 7. Test infrastructure

- **Oracle**: `tests/e2e.sh` → `tests/tui_e2e.py` on a real pty via
  `tests/pty_screen.py`. Helpers: `storm_spawn`, `storm_resize`, `storm_settle`
  (`answer_dsr=`), `AppScrollScreen` (counts app LFS that scroll), `rule_rows`,
  `scrollback_rule_rows`, `Screen(..., bottom_anchor=..., hard_breaks=...)`.
- **Unit/golden**: `tests/tui_test.c` + `src/tui/tui_test.h` hooks
  (`agentc_tui_test_resize`, `_resize_mid_frame`, `_seed_park`, `_seed_pinned`,
  `_set_dsr_reply`, `_region_reflow_h`, `_region_row_width`, …). Golden
  `tests/data/tui_test.expected` (regenerate with stderr merged ONLY after
  `tui tests: ok`).
- **Real-engine measurement**: use `@xterm/headless` 6.1.0-beta.288 from the
  VS Code server (exact path in `evidence/N44-measure-reflow.txt`) for reflow
  questions; tmux 3.6a as a second model. **Measure before modelling** — the
  emulator has repeatedly encoded wrong assumptions.
- **Real-terminal instrumentation**: `AGENTC_TUI_WRITE_LOG=/tmp/agentc-tui.log`
  logs `@ resize apply`, `@ dsr resize class=… r=… park=…`, `@ resize flush …`,
  `@ resize content-anchor … origin=… top=… ce=… full=…`, `@ frame in …`, and
  raw `W <bytes>`.
- **Gates** (both must be green): `env HOME=/tmp/agentc-ci-home make check` and
  `env HOME=/tmp/agentc-ci-home ./tests/e2e.sh`; plus
  `env HOME=/tmp/agentc-ci-home AGENTC_GHOST_REPRO=1 ./build/test/tui_test` must
  have ZERO `=0` labels.

## 8. The precise next step (debug plan)

1. **Read the `ce` computation in `inline_resize_erase`** (the content-anchor
   block, ~`src/tui/tui.c` lines 1180–1260) and state whether `ce` is on-screen or
   pre-scroll. Confirm against the log (`ce=30, full=30, H_stale=7, rows=34`).
2. **Measure the real composer-row reflow** with `@xterm/headless` — DONE in
   `evidence/N52-xterm-band-reflow.txt`: the band extends DOWNWARD from the
   on-screen `ce+1` in every case (fitting, overflow, exact-fill). Reuse
   `/tmp/n52-xterm/probe*.js` or `evidence/N44-measure-reflow.txt`'s method if you
   need to re-confirm.
3. **Make `ce` the true ON-SCREEN content end**: `ce_on = (content_top−1 +
   anchor_total) − max(0, (content_top−1 + anchor_total) + H_stale − rows)`, then
   set the erase origin to `ce_on + 1` and keep `ESC[J` from there. Drop the
   `min(·, rows − H_stale + 1)` term (N52 shows it is unnecessary; on the real
   engine it only ever erases into content because `ce` was pre-scroll).
4. **Regression that would have caught this**: over a SCROLLED transcript, assert
   (i) every committed marker survives, (ii) the computed on-screen content end is
   the same before and after a resize, and (iii) `origin == ce_on + 1` — NOT just
   "2 rules / 0 rules in scrollback". Both `hard_breaks` models. Make it FAIL on
   `e7f8d39`.
5. Keep N51's adaptive probe skip and grow-band fix. Re-run all gates. The
   caller's terminal is the final arbiter: build `make release` and have the
   caller try the drags; read the new `content-anchor` log lines.

## 9. Ops rules (learned the hard way)

- **Bound every command** (`timeout 240 make check`, `timeout 300 ./tests/e2e.sh`,
  `timeout 120 python3 …`). Run slow work in the background to a log and poll with
  `<=5 s` sleeps; if the log stops growing for ~30 s, kill and diagnose. Python pty
  repros must self-cap and always kill their child and `os._exit`. A prior session
  was killed for a stalled unbounded script.
- **Single implementer per tree.** Two concurrent agents corrupted/duplicated work.
- Land only when both gates are green and the gated unit run has zero `=0`; else
  revert `src`/`tests` to the last green commit and record the falsification with
  raw evidence. Never land a lossy trade-off.

## 10. Quick orientation commands

```text
cd /home/pvl/spaces/abird/src/agentc
git log --oneline -8            # e7f8d39 N51, d4d3016 N50, cb18823 N49, fedde27 N46, …
env HOME=/tmp/agentc-ci-home make check
env HOME=/tmp/agentc-ci-home ./tests/e2e.sh
env HOME=/tmp/agentc-ci-home AGENTC_GHOST_REPRO=1 ./build/test/tui_test | grep -c '=0$'
grep -a 'content-anchor\|class=\|resize flush' /tmp/agentc-tui.log | tail -30
```
