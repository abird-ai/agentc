# TODO — P5 tool engine (external rg/fd/grep/find vs built-in)

Plan: P5-TOOL-ENGINE — backend-selectable `grep`/`find`/`ls` (`external` prefers rg/fd/grep/find, built-in last resort)
Status: DONE
Checkpoint: `5e99ca9` (user-approved snapshot; supersedes all earlier evidence)
Design contract: `.agents/design/20-core-agent.md` §4.5; engine pointer `.agents/design/30-extensibility.md` §3.1
Decisions: T-D1…T-D12 — `.agents/plans/P5-TOOL-ENGINE.md` §3

## Committed
<!-- append-only, oldest first; T1/T2/R1–R5 landed pre-rewrite (`refs/tags/backup/pre-rewrite`); mainline carries the squashed baseline `2391ccd` plus the fixes below. -->
- [x] T1 grep + T2 find — engine-selected backends (`rg`→POSIX `grep`→built-in; `fd`→POSIX `find`→built-in), `os_which`, `--tools-engine`/`tools.engine`/`AGENTC_TOOLS_ENGINE`, generated per-backend descriptions, `engine_test` golden + e2e smoke — provenance: `01d9b1c` · design/20 §4.5
- [x] R1/R2 — table-driven descriptor table as the single source of truth + pinned per-backend descriptions — provenance: `faa1232` · after: `2cd0c44` (small tool arenas, portable POSIX-grep options, reader polish)
- [x] R5 — normalize find output + disambiguate POSIX grep (`-Z` NUL records, first-boundary fallback) — provenance: `cf2a35d` · after: `2d093d1` (R5a probe + POSIX-find hardening) · design/20 §4.5
- [x] T3 close-out docs — R5a hardening record — provenance: `138672c` · after: `bfdcba6` (find pattern-translation wording)
- [x] Mainline baseline carries the engine + built-ins + `engine_test`/`tools_test` goldens — provenance: `2391ccd`
- [x] Tool-result fixes — no silent truncation, bounded spill/steps — provenance: `49c450f` · after: `5f1d8bd` (EINTR retry, NUL-preserving results)
- [x] Engine/job-driver hardening — reentrancy, >32-call tool association, engine uniformity — provenance: `33fd7f9` · after: `e9b7676` (drop ambiguous POSIX grep context parse)

## Uncommitted
- [ ] none — every T-D1…T-D12 item is committed or deferred; new work is appended here.

## Deferred
- [ ] external `ls` (`fd --max-depth 1` variant) — no portable external `ls` (locale/format, Windows `dir`); pi keeps `ls` in-process; T-D3/T-D12, design/20 §4.5
- [ ] per-tool engine overrides (`tools.engine.grep`), `rg` config-file support, GNU/BSD feature probing beyond `-Z`, caches/accelerator packs — T-D12
- [ ] full `glob`→`fd --glob` translation beyond the common cases; pi's download fallback — never (agentc never downloads a tool); T-D12, design/20 §4.5
- [ ] pi-style activation defaults (`read/bash/edit/write` only) — activation policy out of scope, `default_tools` unchanged; T-D12
- [ ] exact cross-backend parity — `.gitignore`/hidden semantics and regex dialects differ; stated as honest deltas in the generated description; T-D9, design/20 §4.5
- [ ] POSIX `grep` filenames containing `:`/`-<digits>-` — best-effort parser where `-Z` is unavailable; `rg` recommended; T-D11, design/20 §4.5
