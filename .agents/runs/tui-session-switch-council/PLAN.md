# tui-session-switch-council — plan

Chair: main agent. Scope: a fresh review of the session's diff plus three new
requirements, delivered as one clean, composable design.

## Review findings to fix (from three independent reviewers)
- **High regression:** `main.c` cancels `--continue`/`--resume` for non-tty runs
  (the `cont = resume = false` reset sits outside the tty gate) -> starts a new
  session. Move it inside; docs claim non-tty keeps the newest.
- **Med:** the standalone resume picker's loop calls `term_read` before polling;
  on a real tty `term_read` blocks, so a lone Escape never fires (`input_idle`
  unreachable). Rework the loop (poll-first, idle each tick, resize, poll errors).
- **Low:** `pick_key` keeps processing keys after `done` (result overwrite);
  missing signal handlers around the raw-mode picker; no resize handling;
  `picked_session` leak; `tui_model_pick_open` leaves a stale `menu_filter`.
- **UX consistency:** `/thinking <lvl>` and `/clear` were allowed mid-run while
  every other command refuses.

## New requirements
1. `/resume` and `/continue` available in the TUI at any point, switching
   sessions after cleanly cancelling the current run (tools/jobs/requests) so the
   swap never races an executing tool.
2. Up/Down wrap around in the picker.
3. Default thinking level is `medium`, not `off`.

## Design (composable, one picker)
- New `src/tui/picklist.{c,h}`: the shared list mechanic (filter, scroll,
  wrap-around, keys). Used by every picker.
- Retire the standalone `src/tui/pick.{c,h}` and the pre-TUI `--continue` picker
  in `main.c`. `--continue`/`--resume` on a tty start the TUI and immediately
  open the in-TUI session picker (Esc keeps the mode's newest). One picker.
- `AgcTuiApp` becomes the app-services table: `new_session`, `resume_session`,
  `session_dir`, `on_compact` (folds the old `on_compact`/`on_compact_ud` args).
- `agentc_mode_resume_session(c, path)` swaps to an existing session (open,
  rebind, replace the agent transcript, resync the flush index, emit
  `session_start("resume")`).
- `/resume`/`/continue` while a run is in flight set a pending switch and abort;
  the swap runs after the submit unwinds (idle), so tools/requests are already
  cancelled.
- Pickers: MODEL | THINKING | SESSION kinds over one `PickList`; owned row names
  and paths so no borrowed catalog/session pointer can dangle.

## Chair
- Owns all code, tests/goldens, docs, build, `make check`, `e2e.sh`, review, commit.
