# tui-commands-council — plan

Chair: main agent. Goal: bring agentc's TUI to parity with `../opcode` for the
remaining slash commands, cleanly and robustly:

- `/thinking` with no argument opens an interactive picker (same modal list as
  `/model`); `/thinking <off|low|medium|high>` still sets directly.
- `/compact` runs a manual compaction and reports the result.
- `/new` starts a real new session (new file, rebound persistence, cleared
  agent transcript and view), not just a cleared view.

Reference (assembly sibling, `/home/pvl/spaces/abird/src/opcode`):
`tui_cmd_new` aborts+waits, clears transcript/editor/chat, `tui_session_reset()`;
`tui_cmd_thinking` reports/sets; `tui_cmd_compact` calls `agent_compact()` and
reports done/none/busy. agentc already has `/new` (view-only), `/thinking`
(direct only), no `/compact`; RPC already implements `new_session` and
`compact`, so the core plumbing exists and should be shared, not duplicated.

No worktrees, operate in-tree. Streams own disjoint files and MUST NOT run
`make`/`make check` (one build dir); the chair builds, regenerates goldens and
runs the suite after all streams land.

## Streams

- **A (mode):** `src/app/mode.h`, `src/app/mode.c`, `src/app/mode_rpc.c`.
  Factor the RPC `new_session` body into `int agentc_mode_new_session(AgcModeCtx*)`
  (move the `session_before_switch` cancel check with it, returning `-ECANCELED`
  on cancel), and make RPC call it. Keep `modes_test` green.

- **B (tui/app):** `src/tui/tui.h`, `src/tui/tui.c`, `src/app/main.c`.
  Generalize the modal picker to a kind (model | thinking); implement the
  `/thinking` picker, `/compact`, and a real `/new` via an optional app-services
  callback (`AgcTuiApp`), wired in `main.c` to `agentc_mode_new_session`.

## Chair
- Owns tests (`tests/tui_test.c`, `tests/data/tui_test.expected`,
  `tests/modes_test.c` if needed, `tests/tui_e2e.py`), design docs
  (`40-tui.md` §6, `20-core-agent.md` §7.2), build, `make check`, `e2e.sh`,
  review fixes, and the commit.
