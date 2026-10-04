# TODO — tui-session-switch

Plan: in-TUI session switching — `--continue`/`--resume` picker, `/resume`|`/continue` at any point, one shared picker, clean mid-run swap, medium default.
Status: DONE
Checkpoint: `5e99ca9`
Design contract: `.agents/design/20-core-agent.md` §6.3 (resume/continue) / §6.4 (new-session switch); `.agents/design/40-tui.md` §6.1 (pickers) / §6.2 (mid-run switch)

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] Interactive `--continue`/`--resume` session resume picker: `agentc_session_summary()` (timestamp + first user line) plus a standalone modal list; newest first, Esc starts new, non-tty keeps resuming the newest — provenance: `29ce8c4` · after: `ebb7cf0` (retired the standalone session picker), `44a1b18`
- [x] One shared `PickList` (substring filter, wrap-around, paging, modal keys) backs `/model`, `/thinking` and `/resume`; row names/descriptions/paths owned by the TUI — provenance: `ebb7cf0` · after: `44a1b18`
- [x] `/resume`/`/continue` work at any point: a mid-run request sets `switch_pending` and aborts; the picker opens only after the submit unwinds, so no tool/request is live at the swap — provenance: `ebb7cf0`
- [x] `AgcTuiApp` app-services table (`new_session`/`resume_session`/`session_dir`/`pick_session_on_start`/`on_compact`); `agentc_mode_resume_session()` reopens, rebinds, replaces the transcript and resyncs `flushed` — provenance: `ebb7cf0`
- [x] `--continue`/`--resume` open the in-TUI picker before the first prompt (`pick_session_on_start`); `agentc_session_age_label()` shared by the pickers — provenance: `ebb7cf0`
- [x] Default reasoning level is `medium` (app fallback) — provenance: `ebb7cf0` · after: `8c83a1b` (config `default_thinking` also defaults to `medium`; it had forced `off` and overrode the app fallback)

## Uncommitted
- [ ] none — the switch/picker units are landed and green on the checkpoint.

## Deferred
- [ ] `prompt_command.shadow` dedup test asserts via an `/h` filter — kept meaningful; accepted as-is
- [ ] Inline-mode resize ghosts on reflowing terminals — separate plan `.agents/plans/INLINE-RESIZE-DESIGN.md`; not this workstream
