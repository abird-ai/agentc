# TODO — plan index

Every plan lives in `.agents/plans/<plan>/TODO.md`. The status of each plan is
one line here. `[x]` = done and closed, `[~]` = partial (open items remain),
`[ ]` = not started. The checkpoint `5e99ca9` is the user-approved snapshot:
its commit is the proof for everything recorded as done before it. See
`.agents/plans/TODO-SPEC.md` for the model and item format.

- [~] **inline-panel** — inline panel → resize ghosts → alt-screen-only TUI — `.agents/plans/inline-panel/TODO.md` — PARTIAL — `5e99ca9`
- [x] **p5-tool-engine** — external vs built-in `grep`/`find`/`ls` engine — `.agents/plans/p5-tool-engine/TODO.md` — DONE — `5e99ca9`
- [x] **p5-axes** — RPC v1, `libagentc` SDK, framed CBOR protocol/serve/client — `.agents/plans/p5-axes/TODO.md` — DONE — `5e99ca9`
- [x] **p6-runtime** — actors, bus/queue, hooks, trust/approvals/sandbox, sub-agents/deciders, settings/status/voice — `.agents/plans/p6-runtime/TODO.md` — DONE — `5e99ca9`
- [x] **p7** — custom providers, fork/tree, MCP GET stream, extension helpers, virtual models, `client_id` dedupe — `.agents/plans/p7/TODO.md` — DONE — `5e99ca9`
- [x] **model-discovery** — provider model discovery + setup/onboarding picker — `.agents/plans/model-discovery/TODO.md` — DONE — `5e99ca9`
- [x] **tui-commands** — `/model`, `/thinking`, `/compact`, `/new` and menu polish — `.agents/plans/tui-commands/TODO.md` — DONE — `5e99ca9`
- [x] **tui-session-switch** — `--continue`/`--resume`, in-TUI switch, one picker — `.agents/plans/tui-session-switch/TODO.md` — DONE — `5e99ca9`
- [x] **review-fixes** — early fix-review + review-fixes campaigns — `.agents/plans/review-fixes/TODO.md` — DONE — `5e99ca9`
- [x] **review-hardening** — fresh review WS1–WS7 + verification-fix waves — `.agents/plans/review-hardening/TODO.md` — DONE — `5e99ca9`
- [x] **review-p8** — P8 adversarial review (H1–H3, M1–M11, L1–L11) resolution — `.agents/plans/review-p8/TODO.md` — DONE — `5e99ca9`

## How to add a plan

1. Create `.agents/plans/<plan>/TODO.md` from the template in
   `.agents/plans/TODO-SPEC.md`; add its line here with `[ ]`.
2. Work it from `.agents/runs/<plan>-<agent-id>/TODO.md`.
3. On close, set `[x]` here with the closing sha.
