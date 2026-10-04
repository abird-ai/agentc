# TODO — P7

Plan: P7 feature set — custom providers, sessions/fork/tree, MCP GET, extension helpers, virtual models, `client_id` dedupe
Status: DONE
Checkpoint: `5e99ca9`
Design contract: `.agents/design/20-core-agent.md` (§2.1–2.2, §3.7, §5.4, §6.3), `.agents/design/30-extensibility.md` (§1.1, §2.1), `.agents/design/40-tui.md` (§6.3), `.agents/design/61-protocol.md` (§7.1)
Decisions: P7-D1…P7-D5 (council chair; run record removed; checkpoint `5e99ca9`)

## Committed
<!-- oldest first; newest work at the bottom -->
- [x] token-budget soft cap (P6-SURFACE §18 #23) — provenance: `c2fa1d9` · after: `fb0f433` (per-turn reset, one notify/turn) · design/20 §2.1
- [x] W0 custom base URL / ad-hoc custom provider + `setup` Custom endpoint — provenance: `783a718` · after: `fb0f433` (fail-closed writers, 0600, bounded read) · design/20 §5.4
- [x] W5 `client_id` dedupe (`AGENTC_Q_F_DEDUPE`; queue/agent tools + RPC) — provenance: `911a674` · after: `fb0f433` (sound pre-check, `agent` result, replay) · design/20 §2.2
- [x] W1 `/fork`, `parent_session`, `/tree` — provenance: `8a6d62f` · after: `fb0f433` (fresh id, abort-on-append, valid count, self-contained fork) · design/20 §6.3, design/40 §6.3
- [x] W4 virtual models / per-step routing — provenance: `32b425a` · after: `8794ea7`, `08ca3cf`, `fb0f433` (route credentials, alias row, unpacked route) · design/20 §3.7
- [x] W2 optional MCP GET event stream — provenance: `0504c0e` · after: `fb0f433` (head classification, content-type, `Last-Event-ID`) · design/30 §1.1
- [x] W3 extension helper-process isolation — provenance: `bf15239` · after: `2d4d6f1`, `fb0f433` (non-blocking pipes, hook budget exemption, reap dead) · design/30 §2.1
- [x] P7 as-built contracts + `CHANGELOG.md`/`README.md` record — provenance: `1744a3c`
- [x] fresh adversarial review resolutions (W0-1…W5-8) — provenance: `fb0f433` · after: `aed30db` (design refinements: unpacked route, one resolver/emitter, alias clear, self-contained fork)

## Uncommitted
- [ ] none — every P7 item is committed; new work is appended here.

## Deferred
- [ ] `helpers` config/manifest section for `agentc_ext_register_helper()` — first production entry is the embedding SDK; A-L7, design/30 §2.1
- [ ] helper → host callbacks (helper is one-way) — deliberate P7-D4, design/30 §2.1
- [ ] dedupe priority/turn upgrade on a drop (first-write-wins) — documented in `include/message.h`
- [ ] token-budget spend across rebuild/resume; aliases are process-lifetime until reload — documented, design/20 §2.1/§3.7
- [ ] `INLINE-RESIZE-DESIGN.md` Design 2 rewrite — not pursued; ADR-30 inline resize is as-built, design/40 §1.4
- [ ] one-model-step multi-session granularity — documented constraint, design/61 §7.1
