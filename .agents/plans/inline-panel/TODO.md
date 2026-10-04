# TODO — inline panel / resize ghosts

Plan: fixed-top inline panel, then the resize-ghost hunt and the move to an alt-screen-only TUI (agentc)
Status: PARTIAL
Checkpoint: `5e99ca9`
Design contract: `.agents/design/40-tui.md`

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] Fixed-top inline panel line — `23234c1` panel frame/slide/bookkeeping · after: `cf02810` ADR-7 guard, `71be78b`, `39dee27`, `9bde7ff` (ADR-4..8). Branch-only on `panel-wip-s7` (`342cdb2`), not in HEAD.
- [x] S7 moving-top widen fix — `ddad69d` · after: falsified by the S4f critic (V2 height-reclaim seam, V6 widen-reclaim overwrite); S8 rounds failed. Branch-only on `panel-wip-s7`.
- [x] Master fallback to the flowing renderer — `7bb9e50` drops the screen takeover, `2136caf` freezes the scrollback contract, `778775b` resets master to `2136caf` and preserves the panel on `panel-wip-s7` (`342cdb2`) (ADR-9).
- [x] N0 xterm.js drift repro (branch `n-emul` `75952bd`) — `768f1e1` ports the additive emulator modes + ghost e2e block (ADR-10).
- [x] 6a clamp-normalize on height grow — `ba1a28b` (ADR-11).
- [x] N4 explorer E/F/G DSR re-anchor port — `b8de02c` (ADR-13); N5 single-query probe, sticky `dsr_dead` removed — `9ee073d` (ADR-14).
- [x] N7 reply-aware width-change re-anchor — `7cd226a` (ADR-15).
- [x] N10 one-shot shrink takeover — `05c4df1` · after: falsified by the N11 critic (`cd5e631`), reverted by `5620493` (ADR-16 + addendum).
- [x] N13 region-only relative re-anchor — `ed32b19` · after: `920a11e` F2 keep-row guard (ADR-17 + addendum).
- [x] N17 300 ms DSR probe deadline — `585db49` (ADR-18); N19 restore 6a probe semantics / claim late replies — `13709a9` (ADR-19).
- [x] N22 absolute bottom-anchored erase — `f321f9c` · after: falsified by the N23 critic, reverted by `ee494e0` (ADR-21 + addendum).
- [x] N25 re-land the absolute no-reply erase as DEFAULT + `AGENTC_TUI_RESIZE_ABSOLUTE` kill-switch — `b35a8bd` (ADR-22).
- [x] N30 absolute per-row resize repaint, never scroll on resize — `2f08560` (ADR-23); N33 LF-free commit-bearing resize frames — `8db56e6` (ADR-24); N36/N38 no-reply width-grow coverage — `0083388` (ADR-25 + addendum).
- [x] N44 shrink-scroll reset + faithful reflow model (top-anchored law measured) — `63a0d61` (ADR-30).
- [x] N45 flow-then-pin region placement — `8ce4ad7` (ADR-31); N46 content-anchored erase — `fedde27` (ADR-32); N49 silent-path content anchor — `cb18823` (ADR-33).
- [x] N51 adaptive resize-probe skip + grow-band erase origin — `e7f8d39` (ADR-35); N52/N53 on-screen content end — `cae7bdd` (ADR-36); N56 burst race guard + convergence — `678d5bb` (ADR-39).
- [x] N54 resize-storm origin rebase (persist the reflow scroll) — `21356d9` (ADR-37).
- [x] N58 banner-scroll origin rebase at a bottom start — `939a13f` (ADR-41).
- [x] N60 clean resize queue, only the last geometry of a burst draws — `a8af4f2` (ADR-43) · after: N59 `0b438a9` reverted by `bc6a8a1`; N64 `5ce55f1` / N65 `d3a9bf8` settle-DSR erases landed opt-in, default off (ADR-64/65).
- [x] Falsifications kept for the record (no code landed, fail-closed) — N3b absolute re-anchor (ADR-12 ext.), N8 long-transcript width re-anchor (ADR-15 add.), N43 per-committed-row width records (ADR-29), N50 content-anchor origin = region row (ADR-34).
- [x] N81 interactive TUI defaults to the alternate screen + compact exit dump — `de61a25`.
- [x] L1–L3 removal of the inline renderer — `241ffbd` drops the `tui_mode` config key, `4d66054` removes the write-log diagnostics, `dc2af06` removes the dead inline/scrollback renderers. The TUI is now always the alternate screen.
- [x] Exploration record (branch-only, not merged): `explore-ghost` winner `75b5fa0` (classes E/F/G + conservative model-set origin) after `61a290a`, docs `6bd37be` (ADR-13); superseded on master by the N4 port `b8de02c`.

## Uncommitted
- [ ] `.agents/design/40-tui.md` doc drift: §1.2 (presentation modes; "inline is the default"), §1.4 (flow resizes own the terminal reflow), and §2.3 (inline writer) still describe the removed inline/scrollback model — STALE after L1–L3; rewrite to alt-screen-only.
- [ ] ghost-combine exploration residuals (run record removed; recorded here) — real-terminal laws unproven on the six-hypothesis model set: unmodelled reply laws C2-1 (content-shift × absolute-cursor) and C2-2 (drift coefficient `d'=h−1−Δ`); reply-aliasing / lying replies C1-O1, C2-3 (the DSR stream is trusted end-to-end); intrinsic single-reply ambiguity C1-O3/C2-4 (no loss, bounded ghost). No fix exists.
- [ ] N6 tall-stream-through-resize seam (terminal-owned): a live block taller than the window resized mid-stream loses/duplicates rows; the S4g adopt-seam fix was reverted as worse. Scoped out by ADR-6/7; needs terminal-assisted re-anchor (REPORT residual 1).
- [ ] N57 one-row reflow over-count (ADR-40): `cm`/`anchor_w` diagnostics landed but the too-wide emitted row is still unnamed — the last known ghost source.
- [ ] Decide the fate of the inline-only opt-ins N64 `5ce55f1` / N65 `d3a9bf8`: now dead code paths on an always-alt-screen TUI; remove with the L-series cleanup or keep as history.

## Deferred
- [ ] Accepted fail-closed class: top-anchored + no-reply committed-row loss, covered by `AGENTC_TUI_RESIZE_ABSOLUTE=0` — no family discriminator exists without the reply; the safest partial is a class-G under-clear (ADR-21 addendum / ADR-22).
- [ ] Real-terminal coverage: Alacritty, kitty, VTE, Windows Terminal, X11 xterm, stable xterm.js and the reporter's exact build remain unmeasured (ghost-combine exploration §7.6).
- [ ] Probe performance/soak under heavy load unmeasured; non-inline modes and Windows not exercised (ghost-combine exploration §7.7–7.8).
