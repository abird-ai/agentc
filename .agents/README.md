# .agents — agent-facing material

Everything in this directory is written for agents and maintainers working *on*
`agentc`, not for people using it. None of it is baked into the binary and none
of it ships in the release tarballs (those carry the binary plus `README.md`,
`LICENSE` and `THIRD_PARTY.md`).

```text
AGENTS.md            working agreement for coding agents: hard rules, layout, gates
design/              current-state contracts (the code's source of truth)
plans/TODO-SPEC.md   the handoff / evidence / archival model — READ THIS before opening a plan
plans/TODO.md        plan index: one line per plan
plans/<plan>/TODO.md the plan's committed + uncommitted checklist
plans/P5-*.md        as-built P5 plan + decision detail (referenced by design/)
plans/P6-*.md        as-built P6 plan + decision detail (referenced by design/)
plans/P4-*.md, EXTENSION-*.md, PI-*.md, PORT-FROM-FC.md, HANDOFF*.md,
plans/P6-HANDOFF.md  historical plan/decision record kept for provenance
plans/INLINE-RESIZE-DESIGN.md  the inline-resize defect/design record
runs/                in-flight agent working TODOs only (runs/<plan>-<agent-id>/TODO.md)
```

## Handoff, evidence and archival

The commit is the evidence. There is no separate evidence tree: a checked item
plus its provenance commit id is the whole record, and a user-approved checkpoint
commit is the accepted snapshot. The full model is in
**`.agents/plans/TODO-SPEC.md`** — read it before starting or handing off a plan.

In short: `plans/TODO.md` is the plan index; each plan lives in one
`plans/<plan>/TODO.md` with `Committed` (checked items + `provenance:` sha +
`after:`) and `Uncommitted` sections; parallel agents keep
`runs/<plan>-<agent-id>/TODO.md` and append finished items back into the plan on
completion. Evidence files, run logs and subagent contracts are not kept.

The plans closed at checkpoint **`5e99ca9`** (user-approved, `master` HEAD at the
time): the whole P5 tool-engine / P5-AXES arc, the P6 runtime, P7, the TUI
command/session/picker work, model discovery, the review/fix campaigns, and the
inline-panel/resize-ghost story. See `plans/TODO.md`.

## Current-state contracts (`design/`)

These describe the implementation and are kept in sync with the code; when a
decision or behaviour changes, change the code and the matching design doc in
the same commit.

```text
design/00-architecture.md  principles, constraints, component map, budget
design/10-platform.md      Layer 0 (plat.h) and Layer 1 (net.h) contracts
design/20-core-agent.md    loop, messages, tools (incl. the tool engine), sessions, providers
design/30-extensibility.md MCP, the C extension ABI, skills/prompts/themes, dynamic loading
design/40-tui.md           terminal UI contracts
design/50-build-and-test.md build matrix, tests, platforms, release
design/60-rpc.md           RPC v1 record layer (--mode rpc)
design/61-protocol.md      framed CBOR protocol, agentc serve/client, multi-session
design/70-sdk.md           libagentc SDK, archive composition, packaging
```

`design/` cross-references the as-built `plans/P5-*.md` and `plans/P6-*.md` for
the full decision detail (R-D1…R-D31, C-D1…C-D14, T-D1…T-D12, A/B/C decisions).
Those plan files are kept as the durable decision record; the per-plan `TODO.md`
is the status/summary.

## Conventions

- Paths inside these files are relative to the repository root unless they start
  with `../`; code links are written as `` `path/to/file.c` `` so they stay
  greppable.
- `plans/TODO.md` + `plans/<plan>/TODO.md` are the progress record; never add a
  private evidence file or a second status log.
- Commit with `git -c commit.gpgsign=false commit`.

`agentc` is built with heavy inspiration from
[pi](https://github.com/earendil-works/pi) (MIT); the design record notes where an
idea comes from pi and where we diverge.
