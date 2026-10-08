# .agents — agent-facing material

Everything in this directory is written for agents and maintainers working *on*
`agentc`, not for people using it. None of it is baked into the binary and none
of it ships in the release tarballs (those carry the binary plus `README.md`,
`LICENSE` and `THIRD_PARTY.md`).

```
AGENTS.md                  working agreement for coding agents: hard rules, layout, gates
design/00-architecture.md  principles, constraints, component map, budget
design/10-platform.md      Layer 0 (plat.h) and Layer 1 (net.h) contracts
design/20-core-agent.md    loop, messages, tools (incl. the tool engine), sessions, providers
design/30-extensibility.md MCP, the C extension ABI, skills/prompts/themes, dynamic loading
design/40-tui.md           terminal UI contracts
design/50-build-and-test.md build matrix, tests, platforms, release
plans/P5-AXES.md           forward plan: RPC v1, libagentc SDK, framed CBOR protocol
plans/P5-TOOL-ENGINE.md    the tool-engine axis (as-built behaviour + decisions)
plans/HANDOFF.md           session handoff: model discovery + TUI pickers/commands + open items
plans/INLINE-RESIZE-DESIGN.md  the inline-resize defect and the chosen Design 2 fix
```

Conventions:

- Paths inside these files are relative to the repository root unless they start
  with `../`; code links are written as `` `path/to/file.c` `` so they stay
  greppable.
- `design/NN-*.md` files describe the current implementation and are kept in sync
  with the code; when a decision or behaviour changes, change the code and the
  matching design doc in the same commit.
- `plans/` holds forward-looking work that is not yet part of the implementation.
  When a plan lands, fold its outcome into the matching `design/` document.

`agentc` is built with heavy inspiration from
[pi](https://github.com/earendil-works/pi) (MIT); the design record notes where an
idea comes from pi and where we diverge.
