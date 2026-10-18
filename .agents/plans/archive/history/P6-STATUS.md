# P6-STATUS — Status lines in every region (chrome + per-step)

Status: **as-built (P6-A..E + completion passes 2a-2g).** Status lines in every region.
As-built note: the outcome is folded into `.agents/design/40-tui.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Goal: let an extension place a **status line** in any of these places:

- **before the composer** (above the input box)
- **after the composer** (between the input box and the footer)
- the **status row** (left / right) and **extra rows below** it
- **at the end of every step**
- **after a step's final output** (the turn's last assistant message)

Today the status registry has one row (`LEFT`/`RIGHT`) with providers polled
each snapshot. This generalizes it to **regions** + **lifetimes** — a service
over the existing status registry and the `step`/`output` boundaries, no new
primitive.

## 1. Two natures, one API

| nature | regions | when rendered | lifetime |
| --- | --- | --- | --- |
| **chrome** (persistent UI) | above/below composer, status left/right, below status | every snapshot (version-driven) | `PERSISTENT` |
| **event line** (transient, per step/turn) | step-end, turn-end, final-output | once at the boundary | `PER_TURN` / `ONCE` |

Both are registered the same way; only `region` + `lifetime` differ. Chrome is
polled; an event line is invoked at the step/turn boundary and appended as a
**notice entry** (so it lands in scrollback and can persist to the session).

## 2. Regions

```c
typedef enum {
    AGENTC_REGION_ABOVE_COMPOSER = 0,  /* rows above the input box */
    AGENTC_REGION_BELOW_COMPOSER,      /* rows between input box and status row */
    AGENTC_REGION_STATUS_LEFT,         /* the footer row, left */
    AGENTC_REGION_STATUS_RIGHT,        /* the footer row, right */
    AGENTC_REGION_BELOW_STATUS,        /* extra rows below the footer */
    AGENTC_REGION_STEP_END,            /* after each model step */
    AGENTC_REGION_TURN_END,            /* after each turn (user cycle) */
    AGENTC_REGION_FINAL_OUTPUT,        /* after the turn's final output */
} AgcStatusRegion;
```

TUI placement:

```text
┌───────────────────────────────────────────────────────────────┐
│ transcript / scrollback                                        │
│ …                                                              │
│ [STEP_END lines — appended after each step]                    │
│ [TURN_END lines — appended after the final assistant message]   │
├───────────────────────────────────────────────────────────────┤
│ [ABOVE_COMPOSER rows]                                          │
│ > composer (input)                                            │
│ [BELOW_COMPOSER rows]                                          │
│ [STATUS_LEFT …             STATUS_RIGHT]                       │
│ [BELOW_STATUS rows]                                            │
└───────────────────────────────────────────────────────────────┘
```

## 3. Lifetimes

```c
#define AGENTC_STATUS_PERSISTENT 0u  /* chrome; exists while registered */
#define AGENTC_STATUS_PER_TURN   1u  /* re-rendered each step; empty = nothing */
#define AGENTC_STATUS_ONCE       2u  /* rendered the first time, then empty */
```

- `PERSISTENT` is only valid for chrome regions.
- `PER_TURN` / `ONCE` are valid for `STEP_END` / `TURN_END` (and may be used for
  chrome to show a one-shot hint that later disappears).

## 4. The API (one contribution)

This replaces the old provider/segment pair (`AgcExtStatusProvider` /
`AgcExtStatusSegment`), a deliberate breaking change.

```c
typedef struct AgcExtStatusCtx {
    uint32_t    struct_size;
    int         region;      /* which region is being rendered */
    uint64_t    turn;        /* turn index (TURN_END / FINAL_OUTPUT) */
    uint64_t    step;        /* step index (STEP_END) */
    int         stop_reason; /* AGENTC_STOP_* (event regions) */
    int         final;       /* final output / turn end */
    const char *text;        /* final assistant text (FINAL_OUTPUT), else NULL */
} AgcExtStatusCtx;

typedef struct AgcExtStatusLine {
    uint32_t    struct_size;
    int         region;      /* AgcStatusRegion */
    int         lifetime;    /* AGENTC_STATUS_* */
    int32_t     priority;    /* ordering within a region */
    uint32_t    style;       /* AGENTC_PSEG_STYLE_* */
    void       *ud;
    /* Append one or more lines (UTF-8, '\n'-terminated) to `out` via
     * host->out_write; return 0. Empty output renders nothing. */
    int (*render)(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out);
} AgcExtStatusLine;

void (*add_status_line)(const AgcExtHost *, const AgcExtStatusLine *);
```

- A contribution may emit **multiple lines** (the whole point of "multiple
  status lines"); each line becomes a row in its region.
- Re-adding the same `region` + key replaces the prior contribution.
- Remove: `agentc_status_remove(region, render, ud)`; `agentc_status_reset()`
  clears all. Returning empty output makes a line disappear.

## 5. Rendering model

**Chrome (`PERSISTENT`)** — polled, version-driven, exactly like today's status
row:

- The TUI calls `agentc_status_snapshot(region, …)` when the version changed;
  providers render into their region; rows are ordered by
  `(region, priority, registration)`, then clipped to the region's max rows and
  the terminal width.
- Bounds: `AGENTC_STATUS_MAX_LINES_PER_REGION` (proposed 3) and a global row
  cap; text width clipped; providers over their time budget are disabled.

**Event (`STEP_END` / `TURN_END` / `FINAL_OUTPUT`)** — invoked once at the boundary:

- The core calls `agentc_status_emit(AGENTC_REGION_STEP_END, &ctx)` after each
  model step, `agentc_status_emit(AGENTC_REGION_TURN_END, &ctx)` after each turn,
  and `agentc_status_emit(AGENTC_REGION_FINAL_OUTPUT, &ctx)` after the turn's
  final assistant message (and on `agent_end`).
- The rendered lines are appended to the transcript/scrollback as a **notice
  entry** (`{"type":"custom","name":"notice","region":…,"text":…}`), so they
  persist and replay; the TUI may also show them transiently.
- `ctx` carries `step`, `stop_reason`, `final` and the final `text`, so a line
  can say e.g. "✓ step 3 — 12s, 4 tools" or echo a summary of the last output.

A `steps`/`output` hook (`P6-HOOKS`) is the imperative equivalent if a line
needs to be computed with logic; the declarative `add_status_line` is the norm.

## 6. Example

```c
static int git_branch(const AgcExtHost *h, void *ud, const AgcExtStatusCtx *c, void *out) {
    (void)ud; (void)c;
    h->out_write(out, "main* … ", 9);   /* ABOVE_COMPOSER */
    return 0;
}
static int turn_footer(const AgcExtHost *h, void *ud, const AgcExtStatusCtx *c, void *out) {
    char line[128];
    int n = agentc_snprintf(line, sizeof line, "— step %llu: %s —\n",
                            (unsigned long long)c->step,
                            c->stop_reason == AGENTC_STOP_STOP ? "done" : "tools");
    h->out_write(out, line, (size_t)n);
    return 0;
}
/* init: */
h->add_status_line(h, &(AgcExtStatusLine){ .struct_size=sizeof(AgcExtStatusLine),
    .region=AGENTC_REGION_ABOVE_COMPOSER, .lifetime=AGENTC_STATUS_PERSISTENT,
    .priority=10, .style=AGENTC_PSEG_STYLE_ACCENT, .render=git_branch });
h->add_status_line(h, &(AgcExtStatusLine){ .struct_size=sizeof(AgcExtStatusLine),
    .region=AGENTC_REGION_TURN_END, .lifetime=AGENTC_STATUS_PER_TURN,
    .priority=0, .style=AGENTC_PSEG_STYLE_DIM, .render=turn_footer });
```

## 7. Core and TUI changes

- **Status registry** (`src/core/status.c`): key providers by `region`; add
  `agentc_status_snapshot(region, …)` and `agentc_status_emit(region, ctx)`;
  remove the single-row `slot` model. Keep `agentc_status_version`/`invalidate`.
- **TUI** (`src/tui/`): render the new regions around the composer and footer;
  append `STEP_END`/`TURN_END` lines to the transcript.
- **Step boundary**: call `agentc_status_emit(STEP_END)` after `step_end`;
  `agentc_status_emit(TURN_END)` at final output / `agent_end`.
- **Session**: event lines persist as `custom` `notice` entries (v1, no version
  bump).

## 8. Minimality

No new primitive. This is the existing status registry generalized to
**regions** and **lifetimes**, plus emission at the existing step/run
boundaries; event lines reuse the transcript-entry path. Bounds and validation
are the same discipline as today (drop malformed, clip width, cap rows).

## 9. Handoff

> Replace `AgcExtStatusProvider`/`AgcExtStatusSegment` with `AgcExtStatusLine` +
> `AgcExtStatusCtx` and `add_status_line` in `include/agentc_ext.h` (breaking,
> no alias; user-approved). Update `include/status.h` + `src/core/status.c` to
> key by region and add `agentc_status_snapshot(region,…)` /
> `agentc_status_emit(region,ctx)`; call `emit(STEP_END)` and `emit(TURN_END)` at
> the loop boundaries; render the new regions and append event lines in
> `src/tui/`; persist event lines as `custom` `notice` entries. No new
> primitive. Tests: `tests/status_test.c` (per-region ordering/bounds, chrome
> snapshot, step/run emission with ctx, multi-line clip, removal, replay).
