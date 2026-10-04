# TODO — tui-commands

Plan: TUI slash-command parity — interactive `/model`, `/thinking`, `/compact`, a real `/new`, and command-menu polish.
Status: DONE
Checkpoint: `5e99ca9`
Design contract: `.agents/design/40-tui.md` §6 (menu) / §6.1 (pickers) / §6.3 (`/compact`, `/new`); `.agents/design/20-core-agent.md` §6.4 (new-session switch)

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] `/model` with no argument opens a modal picker over the current provider's runtime catalog, restricted to the current model's wire; full-width reverse-band selection; `/model <id>` sets directly — provenance: `ae6a52e` · after: `44a1b18`
- [x] `/thinking` with no argument opens the level picker (marks the live level); `/compact` runs `agentc_agent_compact()` and reports done/nothing/error; `/new` starts a real session — provenance: `bfed267` · after: `ebb7cf0`, `44a1b18`
- [x] `agentc_mode_new_session()` factored out of the RPC `new_session` body (cancel-veto moved with it); the stdout session header stays RPC-only — provenance: `bfed267`
- [x] Slash-command menu `Up`/`Down` wraps first↔last, matching every picker — provenance: `8c83a1b`
- [x] Picker hardening: owned 128-byte `pick_id` keeps full model ids/aliases; `(no matches)` placeholder keeps an empty-filter pane alive; bracketed paste feeds the filter; markdown tabs expand to the grid stop — provenance: `44a1b18`

## Uncommitted
- [ ] none — commands and pickers are landed and green on the checkpoint.

## Deferred
- [ ] `session_before_compact` cancel reports as "nothing to compact"; a distinct "cancelled" message would be clearer — low
- [ ] `/compact` runs synchronously with no busy indicator — low
- [ ] Inline-mode resize ghosts (`\x1b[J` erase is not a real fix) — separate plan `.agents/plans/INLINE-RESIZE-DESIGN.md` (Design 2); not this checklist
