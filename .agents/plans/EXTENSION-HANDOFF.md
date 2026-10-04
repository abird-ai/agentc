# Handoff — agentc extension architecture (master worklog)

**Read this first.** This is the single entry point for any agent or maintainer
picking up the `agentc` extension work. It records the goal, the complete design
journey, every decision and its rationale, the implementation work items for
phases P1–P4.5 and the post-P4 close-out, the current authoritative
architecture, the exact verification results, the remaining work (close-out
and demand-gated hardening only), the environment and hard rules, and a ready
starting prompt for the next agent.

Status: **P1 (ABI + registry), P2 (async MCP + recompose, incl. the fix wave
D46–D52), P3 (hooks, incl. wave 4; D52 resolved + D53–D66), and the post-review
fix waves R1–R3b are fully implemented and delivered.** The first three phases
are committed on `master` in three logical units: `6c75489` ABI/registry/default
extensions, `2329714` core/app hook consumers, and `6a524cb` docs (this file).
The final extension tree passed the full gate set and an independent P3
acceptance audit (§8). Nothing is pushed; the plan files under `.agents/plans/`
are now tracked. The R1–R3b fix waves (plus the F1 hardening) are committed as `c7e45bb`
`fix(ext): close the post-review findings (R1-R3)`, with this docs commit on
top; nothing is pushed. The recorded post-fix gate run and an independent
post-fix verification are complete (§8). **P4.1 (typed custom providers;
D81–D96 below; design record `.agents/plans/P4.1-PROVIDERS.md`; inventory
§5.5) is independently verified and committed (`f5de256` plus the docs commit
`982fe3e`). P4.2 (skills/prompts/themes + MCP prompts/resources; D97–D109;
inventory §5.6) is implemented, independently verified and committed (feature
`c134625`, docs `c8d18a7`); its clean-tree gate run and the audit's minor fixes
are recorded in §8. P4.3 (asynchronous extension tools; D110–D124; design and
delivery record `.agents/plans/P4.3-ASYNC-TOOLS.md`; inventory §5.7) is
implemented, independently verified and committed (the feature commit plus this
docs commit); the audit's two memory-safety fixes and the recorded gate run are
in §8.** **P4.4 (reserved points + completion; D125–D137; design/delivery
record `.agents/plans/P4.4-COMPLETION.md`; inventory §5.8) is implemented,
independently verified and committed (the feature commit plus this docs commit);
the audit's fail-closed fix and the recorded gate run are in §8. Nothing is
pushed.** **P4.5 (optional dynamic extension loading on macOS/Windows;
D138–D147; design/delivery record `.agents/plans/P4.5-DYNAMIC.md`; inventory
§5.9) is committed (feature `9f4b7db`, docs `9f3c12b`); the platform-runtime
half is gated on the CI jobs. The post-P4 close-out (write-first stdio write,
load guard, RPC observer rebind, dynamic relative-home rule; D148–D151;
inventory §5.10) is delivered in the working tree: `nix develop -c make
wine-check` is 30/30 green, the two known tickets are resolved, and the
Windows/macOS blocking-pipe residual is documented (§9.4). Nothing is pushed.**

Repo: `/home/pvl/spaces/abird/src/agentc` (freestanding C23 coding agent).
pi reference checkout: `/home/pvl/spaces/abird/research/pi` (pi monorepo).

---

## 0. How to get up to context (context-recovery protocol)

If you are a fresh agent with no chat history, do exactly this:

1. Read this file top to bottom. It is deliberately redundant so you do not have
   to reconstruct anything from chat logs.
2. Read `.agents/plans/EXTENSION-SPEC.md` (v3) — the authoritative,
   implementation-ready spec. Everything in the code is downstream of it.
3. Read `.agents/docs/30-extensibility.md` — the **current** extension design
   record (post-P1/P2), and `.agents/docs/20-core-agent.md` §4 (tools) and §6
   (prompt) — the current core design.
4. Skim `.agents/docs/50-roadmap.md` (milestone context) and
   `.agents/docs/API.md` (implementation notes). Read `REVIEW.md` if you are
   touching a reviewed area.
5. Only if you need pi-model background: `.agents/plans/PI-EXTENSION-MAP.md`
   (pi 1.0.0) and `.agents/plans/PI-2-DURABLE-MCP-MAP.md` (pi 2.0 durable/mcp).
6. Superseded exploration, kept for ideas only: `EXTENSION-TOOL-ENGINE.md`,
   `EXTENSION-MINIMAL.md`.
7. Then read the code in this order: `include/agentc_ext.h` (the ABI),
   `include/ext.h` (core-facing API), `src/ext/registry_int.h` (internal bus),
   `src/ext/registry.c` (host + registry + hook bus), `src/ext/linked.c`
   (generated table), then the three default extensions
   (`src/core/tools/registry.c`, `src/core/prompt.c`, `src/ext/mcp.c`).

The golden rule from `.agents/AGENTS.md`: change the code and the design doc in
the same commit; keep `make check`, `./tests/e2e.sh`, `./tests/mcp.sh` and
`./tests/ext.sh` green.

---

## 1. TL;DR

`agentc`'s extension surface was redesigned so that **built-in tools, MCP and
statically-linked extensions share one model**: one ABI header
(`include/agentc_ext.h`), one registry (`src/ext/registry.c`), one hook bus, and
`builtin-tools`/`builtin-context`/`mcp` as ordinary **default extensions**. The
word "plugin" is gone from live code and docs — the convention is
**extension**/`ext`. The design is deliberately minimal in features but complete
in seams: a later extension can build the rest of pi's model (skills, MCP
resources/prompts, permission gates, transforms, continuation, custom providers)
without further core changes.

Phase status (spec §14):

- **P0/P1 — ABI + registry: delivered.** One header, one `ABI_VERSION`, the
  registry/host/hook bus, the three default extensions, prompt sections, status
  types, the convention rename, the Rust crate, both examples, ported tests and
  the live docs.
- **P1-completion — registry semantics: applied in the same change set.**
  `agentc_ext_emit` honors `MERGE_REPLACE`, rejects an `OBSERVE` handler that
  returns `1` with an error log, and dispatches over a true subscriber snapshot;
  the public `add_tool` and `request_recompose` set dirty while commands and
  sections are read live; `agentc_ext_unload`/`agentc_ext_shutdown` remove an
  owner's registrations before `shutdown()` and shutdown ends with
  `agentc_status_reset()`; `set_status` is an auto-clearing owner-tagged status
  segment; `set_model`/`set_thinking` remain optional front-end sinks (the P1
  app installed neither; P3 installs them in `agentc_mode_setup`, D66). `ext_test`
  and `tests/e2e.sh` gained the matching coverage (see §5.1).
- **P2 — async MCP + recompose: delivered.** Non-blocking per-server MCP
  connect/re-sync state machine, pure `agentc_mcp_tools` snapshot, registry pump
  hook, `mcp_servers_change`, app-installed recompose at the turn boundary.
- **P1-hardening / redesign wave: design frozen, implementation applied
  (D35–D45).** Stable tool records; owner rollback covering deferred callbacks
  and pending HTTP requests; the `AgcToolVetoDecision` veto seam (adapter
  consumes `reason` + rewritten `input`; the agent consumer was the first P3
  item, delivered in §5.3); the `fields`-patch contract with `agentc_ext_merge_fields`; the flat
  header patch for `before_provider_headers`; quiet/override guards; corrected
  `rc == 1` chain semantics; `agentc_ext_register` ABI/size validation; the
  internal remove-tool retire seam; the one-server-per-pump-call MCP budget; and
  `extensions.disabled` covering linked extensions. See §5.1.
- **P2 fix wave — MCP completion + build scaling: applied (D46–D52).** MCP
  retiral/replacement reaches the registry (`agentc_ext_remove_tool_internal`;
  publish marks an entry registered only on a successful add), the pump advances
  exactly one server per call (round-robin), an in-flight `list_changed` is
  latched, strict `--tools` defers not-yet-connected `mcp__` names, and
  `extensions.disabled` applies to defaults and linked extensions after
  `register_linked`. The Makefile builds the whole manifest (every C entry;
  Rust from the same manifest) and `gen-exts.py` rejects names that sanitize to
  invalid C identifiers. See §5.2.
- **P3 — hooks: fully delivered.** Core emit sites with `wants` guards;
  override hooks for `tool_call`/`tool_result`/`message_end`/`input`/
  `before_agent_start`/provider points; settle continuation; the
  `tool_result`/`message_end` moves before append/observer; the §8 trust
  reordering + `resources_discover` gating; the §10 destructive-tool default
  (D52 resolved); wave 4 `model_select`/`thinking_level_select` emit sites and
  the app model/thinking sinks. See §5.3.
- **P4.1 — typed custom providers: implemented, independently verified and
  committed** (`f5de256` plus its docs commit). The appended `add_provider`
  host slot, the typed request view + two sinks, core-applied auth, registry
  records with retirement, discovery auth, static models, generic config and
  the `fake_provider` example. See §5.5 and D81–D96.
- **P4.2 — skills/prompts/themes + MCP prompts/resources: implemented,
  independently verified and committed** (feature `c134625`, docs `c8d18a7`).
  One invocable-prompt registry, file templates + `/skill:<name>`, named
  themes + `/theme <name>`, all three `resources_discover` path kinds consumed,
  MCP capability negotiation and per-kind list sync, MCP prompts as `mcp__…`
  commands, and three lazily published MCP resource tools. See §5.6 and
  D97–D109.
- **P4.3 — async extension tools: implemented, independently verified and
  committed.** The appended `AgcExtTool` `start`/`step`/`stop`
  trio, the internal `AgcJob.cleanup` hook, extension-owned state, per-step
  staged output, retire-not-free + `stop(UNLOAD)`, the signal window, the
  per-step pump, `run`-wins, the 30-min async clamp and the Rust mirror; the
  `async_demo` reference and the loop tests. See §5.7 and D110–D123.
- **P4.4 — reserved points + completion: implemented, independently verified
  and committed.** The persistence-owner fix, `tool_call.terminate`,
  `session_before_switch` + `session_start{reason:"new"}`, in-flight extension
  HTTP cancel, MCP idle drain/reconnect and the `mcp_servers_change`
  status/caps payload. See §5.8 and D125–D137.
- **P4.5 — optional dynamic loading on macOS/Windows: implemented, verified
  and committed** (feature `9f4b7db`, docs `9f3c12b`),
  platform-runtime-gated on CI. P4.5a (loader surface + scan/adopt/close
  policy + fake-loader goldens) and P4.5b (`ext-dylib`/`win-ext-dylib`,
  `tests/dyn.sh`, `make dyn-check`, CI jobs) are delivered; the real
  macOS/Windows runtime claims wait on those CI jobs. See §5.9 and D138–D147.
- **Post-P4 close-out — delivered in the working tree.** The write-first MCP
  stdio write (D148), the load guard for `agentc_ext_shutdown` during `init`
  (D149), the RPC observer rebind (D150), the dynamic relative-home refusal
  (D151), the `plat.h` err-on-failure contract and the `PI-EXTENSION-MAP.md`
  column removal. `nix develop -c make wine-check` is 30/30 green; the Windows
  Windows/macOS blocking-pipe residual is documented (D148, §9.4). See §5.10.
- **R1–R3b — post-review fix waves: delivered.** Runtime wiring, CI and prompt
  refresh (R1); registry/core correctness (R2); UX/observability/build/MCP
  memory (R3). See §1.1 and §5.4; committed as `c7e45bb` (docs follow in the
  next commit).

### 1.1 Post-review fix program (R1–R3b) — delivered

A fresh post-P3 review found correctness, on-ramp and documentation drift. Three
fix waves landed on top of the P3 tree; the table below is the final state.

| wave | scope | status |
|---|---|---|
| **R1** | runtime wiring, CI, prompt refresh, extension-doc drift | delivered |
| **R2** | registry/core correctness | delivered |
| **R3** | UX/observability/build/MCP memory | delivered |

**R1 — wiring, CI, prompt, docs.** One `agentc_mode_pump` duty is installed by
`agentc_mode_setup` as both the wire poll hook and the agent pump for every mode
(TUI chains through it and restores it after a submit; the RPC read-wait and the
TUI idle tick call it; `agentc_ext_pump` has a re-entrancy guard). The startup
pump is bounded to 400 ms and exits early once `agentc_mcp_pending()` is false
(or no server record exists). In auto mode (no explicit `--system`) the agent
rebuilds the system prompt from the live tool table before every turn; an
explicit `--system` and a per-run `before_agent_start` `systemPrompt` override
stay fixed, and the effective prompt is re-published to the extension context.
CI runs plain `make`, `make ext-pipeline` and `make check` (which includes
`check-ext`) with apt clang/make/python3, then runs `tests/ext.sh` under
`nix develop`: cargo comes from the flake dev shell (Ubuntu's is too old for the
workspace lockfile), **not** from apt. `build/exts.mk` is a real make target
written atomically by `tools/gen-exts.py`; a generation error fails the build.
The `extensions/README.md` on-ramp, the extension stdout/`--dump-wire` wording
and the docs/20 stale flags were corrected in the same wave.

**R2 — registry/core correctness.** Owner restoration around
hook/tool/command/section/pump/defer/HTTP callbacks with `[ext:<name>] ` log
prefixes and named failure/overrun diagnostics; 3-strike overrun containment (a
clean call resets; the overrunning occurrence is fail-closed on override and
fail-open on observe); unknown hook points rejected at `on`; a live-registration
counter behind `agentc_ext_wants`; emit recursion capped at 32 (override
blocked, observe empty); `set_status` keyed by (owner,key) with 8 live keys; the
`AGENTC_EXT_FIELD_OK`/`AGENTC_EXT_HOST_HAS` gates plus
`AgcExt.required_host_size`; `resources_discover` fail-closed; `message_end`
terminal preservation and `tool_call`-entry validation; `turn_end.continue` on
all non-hard turns; `select_tools` dedupe; a blocked `input` error event and a
negative submit; `agentc_http_set_cancel` and forced `\uXXXX` response-header
JSON; the OAuth refusal-path leak fix.

**R3 — UX/observability/build/MCP memory.** MCP bounded entries
(256/server, 1024 total), over-long names skipped, descriptions truncated,
exposed names deduped by hash inside 64 bytes, the frozen pile capped at 256
where handed-out entries are never freed/reused (the cap fails the server
instead of recycling); HTTP cancel wired; the 400 ms readiness/startup budget;
`--list-extensions`, the post-`load_all` summary, the D52 `--help` note and the
MCP-disabled deferral diagnostic; the TUI UI sink (`notify`; `set_title` falls
back to a notice); the build fragment as a real make target with atomic writes;
retirement of the `AgcWantsFn`/`AgcNotifyFn` bridge in favor of direct registry
calls plus `agentc_ext_append_entry`; `json_fields_apply(base, over,
keep_nulls)`; bounded `load_all` rounds and live shutdown iteration.

---

## 2. Reading order and document index

| order | file | status | what it is |
|---|---|---|---|
| 1 | `plans/EXTENSION-HANDOFF.md` | **this file** | master worklog: journey, decisions, inventory, pending |
| 2 | `plans/EXTENSION-SPEC.md` | **authoritative (v3)** | implementation-ready spec + review disposition appendix |
| — | `plans/P4.1-PROVIDERS.md` | delivered, verified, committed | P4.1 typed-custom-provider design, delivered deviations and deferrals |
| — | `plans/P4.3-ASYNC-TOOLS.md` | delivered, verified (committed) | P4.3 async-tool design, delivered deviations and deferrals |
| — | `plans/P4.4-COMPLETION.md` | delivered, verified (committed) | P4.4 reserved-point/completion design, delivered deviations and deferrals |
| 3 | `docs/30-extensibility.md` | current design record | MCP client, extension ABI/registry, static extensions |
| 4 | `docs/20-core-agent.md` | current design record | core loop, tools (§4), prompt/config (§6) |
| 5 | `docs/50-roadmap.md`, `docs/API.md` | current | milestones, implementation notes |
| 6 | `docs/REVIEW.md`, `docs/00-design.md`, `docs/10-platform.md`, `docs/40-tui.md`, `docs/platform_notes.md` | current | principles, platform, TUI, port notes |
| 7 | `plans/PI-EXTENSION-MAP.md`, `plans/PI-2-DURABLE-MCP-MAP.md` | reference | pi 1.0.0 and pi 2.0 API maps |
| 8 | `plans/EXTENSION-TOOL-ENGINE.md`, `plans/EXTENSION-MINIMAL.md` | superseded | earlier maximal/minimal designs, kept for ideas |
| — | `plans/PLAN.md`, `plans/PORT-FROM-FC.md` | pre-existing | original project plan and the fc→agentc port history |
| — | `AGENTS.md`, `README.md` (this dir) | current | hard rules and the index of this directory |

Everything agent-facing lives under `.agents/`, as required. The `.orchestrator/`
tree is historical port orchestration and is intentionally not updated.

---

## 3. Starting point (W0) — what the code looked like before this work

- `include/agentc_plugin.h`: the native plugin ABI v2
  (`AgcHostV1`/`AgcPluginV1`, `AGENTC_ABI_VERSION 2`, `AGENTC_ABI_VERSION_MIN 1`,
  append-only with `reserved[]` and `offsetof` gates).
- `src/ext/plugin.c` (~1391 lines): the plugin host vtable, static registry
  inclusion, pending-call table, `tool_done`, JSON helpers, commands, context/UI
  sinks, owner rollback via snapshots.
- `src/ext/plugin_int.h`: internal alias of `include/plugin.h`.
- `src/ext/mcp.c` (~1765 lines): MCP stdio + Streamable HTTP client, blocking
  connect at load, `agentc_mcp_tools` re-syncing on `list_changed`.
- `src/core/tools/registry.c`: the `builtins[]` table + `agentc_tools_builtin`.
- `src/core/prompt.c`: `agentc_prompt_build` (preamble/tools/rules/environment).
- `src/core/status.c` + `status_builtin.c`: the status-provider table (v2
  `status_register`).
- `src/app/main.c:544–583`: hand-merged builtins → plugins → MCP tool table.
- `tools/gen-plugins.py`: emitted `build/plugins.c`; `plugins/` held two examples
  (C `hello`, Rust `rust_echo`); `crates/agentc-plugin` the Rust wrapper.

Known deficiencies this work fixes: three parallel tool models, no unregister,
observe-only hooks with a dead `project_trust`, MCP blocking startup, no
extension ordering, and a two-word ("plugin"/"extension") mental model.

---

## 4. The design journey (W0–W14)

Each step names the request and the artifact it produced. These were design-only;
none of it was code until P1.

- **W0 — starting point.** As §3. Documented in `docs/30-extensibility.md`.
- **W1 — initial request.** *"Make MCP and tool calls part of the extension
  architecture so it is more modular; MCP included by default."* Produced
  `EXTENSION-TOOL-ENGINE.md`: one contribution model
  (tools/commands/prompt/resources/status/events), an `AgcExt`/`AgcExtHost`
  interface, core-as-extensions, MCP late/background connect, skills as
  contributions, a hooks section, phases P1–P6.
- **W2 — ABI break + skills.** *"We can break ABI for better redesign if
  fruitful"* / *"how about skills?"* Break once; drop `ABI_MIN`/`reserved[]`/
  offset gates but keep `abi_version`+`struct_size`; skills become prompt/context
  contributions; **MCP prompts are structurally skills** (the unification
  argument). Confirmed MCP advertises `capabilities:{}` and never calls
  `prompts/*` or `resources/*`.
- **W3 — hooks.** Documented the real state: observe + veto only. Emitted observe
  points: `agent_start/end`, `turn_start/end`, `message_*`,
  `tool_execution_*`, `tool_result`, `before_provider_request`,
  `after_provider_response`, `provider_stream_event`, `session_*`,
  `project_trust`. Vetoable: `tool_call`. Reserved-but-never-emitted: `input`,
  `before_agent_start`, `model_select`, `thinking_level_change`,
  `resources_discover`, `mcp_servers_change`. `finishTurn` absent. Added the
  capability taxonomy.
- **W4 — provenance question.** First answer was wrong (claimed the two provider
  events were agentc-only); corrected in W5.
- **W5 — provenance corrected + naming + settle.** Found pi's authoritative
  union in `.../pi-coding-agent/dist/core/extensions/types.d.ts`: pi defines
  `before_provider_request`, `before_provider_headers`, `after_provider_response`,
  `agent_before_settle`, `agent_settled`, `context`, `context_with_system`,
  `cache_warming_decision`, `user_bash`, `ui_prompt_*`, `session_before_*`.
  Established the naming rule (`<subject>_<phase>`) and the adopt/defer table.
- **W6 — full pi map.** `PI-EXTENSION-MAP.md`: 41 events, 31 `pi.*` methods,
  `ctx.*`/`ctx.ui.*`, tool-definition fields, hook result types, gap summary.
- **W7 — pi 2.0 durable + mcp.** `PI-2-DURABLE-MCP-MAP.md`: `pi-durable`
  (Session/Tx/documents, Harness, Conversation, `defineTask`,
  `Registry`+`Extension{tools,sections,hooks,wraps,tasks}`, typed hooks, rich tool
  results) and `pi-mcp` (tools **and resources and prompts**, transports, OAuth,
  in-memory test transport). Key finding: the coding-agent extension API is
  unchanged from 1.0.0; the additions are the two new packages and a second
  extension model inside `pi-durable`.
- **W8 — pivot to minimal.** `EXTENSION-MINIMAL.md` (first form): one tool-source
  seam, MCP connect off startup, recompose at the turn boundary.
- **W9 — minimal but complete API.** Rewrote `EXTENSION-MINIMAL.md`:
  `AgcExt`/`AgcExtHost`, contribution kinds, a uniform JSON hook bus,
  capabilities, core-as-extensions.
- **W10 — veto → override.** Collapsed `OBSERVE|VETO|TRANSFORM|INJECT` to
  **`OBSERVE|OVERRIDE`** (pi's two categories), with pi-shaped hook results.
- **W11 — orchestrator pass, full spec.** Four parallel recon agents (old ABI,
  tool pipeline, MCP, providers/build), then `EXTENSION-SPEC.md` v1.
- **W12 — header hook correction.** `agent.c` already extracts the header block
  and body separately and passes them separately (`agent.c:686–693`/`:721`), so
  `before_provider_headers` runs post-assembly/pre-send with no rejoin.
- **W13 — fresh-eyes review.** Three reviewers (correctness, design, risk)
  produced ~60 findings; most accepted; `EXTENSION-SPEC.md` v2 written with a
  disposition appendix (see §11).
- **W14 — first handoff.** This file's predecessor.
- **W15 — product decisions closed (start of implementation).** Convention
  changed to extension/ext only; default extensions and their one privilege;
  `AgcExt.order` added; O1 (`--system` hard-replace) and O2 (defaults-first)
  confirmed. Recorded in the spec §0 and decisions D20–D22.
- **P1 — implementation (see §5.1).**
- **P2 — implementation (see §5.2).**

---

## 5. Implementation journey

### 5.1 P1 — ABI + registry (delivered)

The work items below are the complete P1 change set.

**New files**

| file | purpose |
|---|---|
| `include/agentc_ext.h` | The only extension ABI (`AGENTC_EXT_ABI 1`): `AgcExtHost` (31 service slots plus `abi_version`/`struct_size`), `AgcExt` (with `int32_t order` and the appended `required_host_size`), `AgcExtTool` (sync `run`), `AgcExtToolCall`, `AgcExtCommand`, `AgcExtSection`, `AgcExtStatusSegment`, `AgcExtStatusProvider`, `AgcExtResult`, `AgcExtHookFn`, tool/hook flags, `AGENTC_HEV_*` point names, `AGENTC_EXT_FIELD_OK`/`AGENTC_EXT_HOST_HAS`. Appended ergonomics kept from the old ABI: `strdup_`, `json_get_str/int/bool`, `json_escape`, `set_title`. |
| `include/ext.h` | Core/app-facing registry API: `agentc_ext_register_defaults/_register/_adopt/_register_linked/_load_all/_unload/_apply_config/_pump`, `_tools/_commands/_run_command/_render_sections`, `_wants/_emit/_tool_veto`, `_dirty/_clear_dirty`, `_emit_agent_event`, `_shutdown`, `_set_context/_set_entry_sink/_set_ui_sink/_set_model_sink/_set_thinking_sink`, the three default-ext getters. |
| `src/ext/registry.c` | Host vtable, extension descriptor table (owner-tagged), tool/command/section registries, status tracking, hook bus (caps, priority, policies, merge algebra, fail modes, budget, `wants`), deferred queue, single pending HTTP request, context/sinks, dirty flag, `agentc_ext_register_defaults`/`_load_all`/`_unload`. |
| `src/ext/registry_int.h` | Internal bus: `agentc_ext_add_tool_internal` (async internal tools), `agentc_ext_add_pump` (periodic default-ext work), plus `ext.h`. |
| `src/ext/linked.c` | Includes generated `build/exts.c` and defines `agentc_ext_register_linked`. |
| `src/core/prompt.h` | `agentc_prompt_build`, `agentc_ext_prompt_build`, `agentc_builtin_context_set`. |
| `tools/gen-exts.py` | Reads `extensions/manifest.json`, emits `build/exts.c` (`{name, entry}` + `agentc_exts_register_all`), and touches `src/ext/linked.c`. |

**Deleted**

`include/agentc_plugin.h`, `include/plugin.h`, `src/ext/plugin.c`,
`src/ext/plugin_int.h`, `tools/gen-plugins.py`.

**Renamed (the one-word convention)**

`plugins/` → `extensions/` (incl. `manifest.json`, `hello/`, `rust_echo/`,
`README.md`); `crates/agentc-plugin/` → `crates/agentc-ext/`;
`tests/plugin_test.c` → `tests/ext_test.c`; `tests/plugin.sh` → `tests/ext.sh`;
`tests/data/plugin_test.expected` → `tests/data/ext_test.expected`.

**Ported / changed**

| file | change |
|---|---|
| `src/core/tools/registry.c` | `builtin-tools` `AgcExt`; registers `builtins[]` via `agentc_ext_add_tool_internal`; keeps the `bash` shell-description rewrite. |
| `src/core/prompt.c` | Four core sections + the `builtin-context` section (`project_context` + `skills`); `--system` hard-replaces. |
| `src/core/status.c`, `src/core/status_builtin.c`, `include/status.h` | `AgcExtStatusSegment`/`AgcExtStatusProvider`; added `agentc_status_remove`. |
| `src/ext/mcp.c` | `mcp` `AgcExt` surface + `agentc_mcp_set_context` (P1 still synchronous). |
| `src/app/main.c` | Composition → `agentc_ext_register_defaults`/`_register_linked`/`_apply_config`/`_load_all` (the P2 fix wave moved `_apply_config` after `_register_linked`; see D50); `agentc_ext_tools`; `agentc_ext_prompt_build`; `agentc_ext_shutdown`. |
| `src/app/mode.c`/`mode.h`, `src/core/agent.c`, `include/agent.h`, `src/tui/tui.c` | Extension bridge/veto/context; `agentc_agent_set_ext_bridge`; TUI command registry calls. |
| `include/config.h`, `src/core/config.c` | `extensions.disabled` → `extensions_disabled`. |
| `src/core/events.c`/`events.h` | Comment/naming alignment only. |
| `Makefile` | `EXT_REG`/`EXT_C`/`EXT_C_DEFS`/`EXT_RUST_DEFS`/`EXT_DEFS`, `linked.o`, `ext-pipeline`, `release-exts`, `src/ext/linked.c`, `extensions/`, `build/exts.c`, `agentc_ext_*_init`. |
| `extensions/hello/hello.c`, `extensions/rust_echo/src/lib.rs`, `crates/agentc-ext/src/lib.rs`, `Cargo.toml`s, `extensions/manifest.json` | Ported to `AgcExt`/`AgcExtHost`; synchronous `run` + `out_write`; no `reserved[]`/offset gates/`ABI_MIN`; crate renamed. |
| `tests/ext_test.c` | Rewritten for the registry + hook bus; fixture `agentc_ext_fixture_init`; harness mode. |
| `tests/status_test.c`, `tests/agent_loop_test.c`, `tests/tui_test.c`, `tests/data/status_test.expected`, `tests/data/ext_test.expected` | Repointed/renamed/regenerated. |
| `tests/tui_e2e.py` | Stale default-model expectation (`claude-sonnet-4-5 (anthropic)` → `gpt-5 (openai)`) — a pre-existing failure from commit `fb81615`, fixed so `e2e.sh` is green. |
| `.agents/docs/*`, `.agents/AGENTS.md`, `.agents/README.md`, `README.md`, `extensions/README.md` | Extension convention; `30-extensibility.md` Layer 2/3 rewritten to the new ABI/registry; `20-core-agent.md` §4/§6 updated. |

**P1-completion pass (same change set).** Closes the P1 registry
semantics that the spec left implicit:

- `agentc_ext_emit` honors each point's declared merge algebra: `replace` points
  take an override result wholesale, `fields` points merge keys (`null` deletes),
  and an `OBSERVE` handler that returns `1` (like one that returns a result) is
  logged and ignored.
- Dispatch walks a true copy of the subscriber list, so `off()` (or a new
  registration) during the walk does not affect the current emit; the next emit
  sees the change.
- The public `add_tool` sets dirty (alongside the internal
  `agentc_ext_add_tool_internal` and `request_recompose`); `add_command`/
  `add_section` do not, because commands and sections are read live.
- `agentc_ext_unload`/`agentc_ext_shutdown` remove an owner's registrations
  before calling `shutdown()`; shutdown ends with `agentc_status_reset()`.
- `set_status` folds into the status table as an auto-clearing, owner-tagged
  segment (spec §6.3). `notify`/`set_title` stay optional UI-sink services;
  `set_model`/`set_thinking` are optional front-end service sinks whose registry
  plumbing `ext_test` covers; the P1 app installed neither (P3 installs them in
  `agentc_mode_setup`, D66).
- Tests: `tests/ext_test.c` adds
  merge/observe/snapshot/dirty/unload/`set_status` coverage; `tests/e2e.sh` adds
  the `--system --dump-wire` hard-replace check. The provider-header
  CRLF/forbidden round-trip was P3 and is delivered (spec §13).
- Spec/doc corrections in this pass: `EXTENSION-SPEC.md` §0/§3.5/§4.7/§5/
  §5.1/§6.1/§6.3/§8/§13/§16, `docs/30-extensibility.md` §3.2/§3.4/§3.5/§4,
  `docs/20-core-agent.md` §8 (the nonexistent `--append-system-prompt`); see
  D34.

**P1-hardening / redesign wave (this change set, applied — D35–D45).** The
frozen target: pointer-stable tool records; owner rollback covering deferred
callbacks and pending HTTP requests (and a lock-held defer-ring reset on
shutdown); the `AgcToolVetoDecision` veto callback seam (the adapter consumes
`reason` + the rewritten `input`; the agent consumer was the first P3 item,
delivered in §5.3); the
`fields`-patch accumulator contract with the new `agentc_ext_merge_fields`
helper; the flat `{ "<Header>": value|null }` shape for
`before_provider_headers`; quiet/override guards (`agentc_events_set_quiet`
never suppresses a declared OVERRIDE point); corrected `rc == 1` chain
semantics; `agentc_ext_register` ABI/size validation and `append_entry` object
validation; the internal
`agentc_ext_remove_tool_internal` retire seam for MCP re-sync; the
one-server-per-pump-call MCP budget; and `extensions.disabled` covering linked
extensions (config applied after `agentc_ext_register_linked()`). The matching
spec/doc sections are `EXTENSION-SPEC.md`
§3.5/§4.1–4.3/§4.6/§4.7/§5/§6.3/§7/§9/§10/§13 and `docs/30-extensibility.md`
§1.1/§3.2/§3.4/§3.5/§4; the wave's tests are listed in spec §13. The follow-up
**P2 fix wave** (§5.2, D46–D52) closes the P2 items these decisions left
pending: retiral reaching the registry, one server per pump call, the
`list_changed` latch, strict `--tools` deferral for `mcp__` names, linked
`extensions.disabled`, and manifest-driven build scaling.

### 5.2 P2 — async MCP + recompose (delivered)

| file | change |
|---|---|
| `src/ext/mcp.c` | Per-server state machine `NEW → INIT_SEND → INIT_WAIT → NOTIFY → SYNC_SEND → SYNC_WAIT → READY \| FAILED`, with re-sync re-entering `SYNC_SEND` when `sync_pending`. `agentc_mcp_start` reads config and creates records (no connect); `agentc_mcp_pump` advances **exactly one server per call, round-robin** (P2 fix wave), one bounded step each (250 ms stdio slice / one bounded HTTP request); `agentc_mcp_tools` is a pure snapshot; re-sync is pump-owned. On a committed `tools/list`, discovered tools are published with `agentc_ext_add_tool_internal` and `mcp_servers_change` is emitted; an entry is marked published only when the add succeeds (a duplicate/failed publish is logged once). A re-sync that retires or replaces a tool withdraws the registry entry with `agentc_ext_remove_tool_internal(name)` before publishing the fresh one, so the next collected table shows the changed description and drops the removed tool. A `list_changed` notification that arrives during an in-flight sync is latched; the follow-up sync runs after the current commit. `agentc_mcp_load` retained as a synchronous start+pump test helper. |
| `include/mcp.h` | Declares `agentc_mcp_start`/`agentc_mcp_pump`; documents the async surface. |
| `src/ext/registry_int.h`, `src/ext/registry.c` | `agentc_ext_add_pump`; `agentc_ext_add_tool_internal` sets dirty; pump callbacks run with their owning extension active so pump-added tools are owner-tagged; `agentc_ext_shutdown` releases every tool record. |
| `include/agent.h`, `src/core/agent.c` | `AgcRecomposeFn` + `agentc_agent_set_recompose`; `recompose_now()` called after a tool batch fully returns and immediately before terminal `agent_end` on abort/error. |
| `src/app/mode.h`, `src/app/mode.c` | `AgcModeConfig.recompose`/`recompose_ud` installed on every agent, including `agentc_mode_rebuild_agent`. |
| `src/app/main.c` | `agentc_policy_recompose` re-applies `--tools`/`default_tools`/`--no-tools` over fresh `agentc_ext_tools`; clears the dirty flag after the startup composition. P2 fix wave: strict `--tools` defers a name beginning `mcp__` that is absent at startup (selected by the recompose once the server connects); any other unknown name still exits 2. |
| `src/ext/registry.c` | P2 fix wave: `agentc_ext_apply_config` disables **default and linked** extensions by name; the app calls it after `agentc_ext_register_linked()`; `agentc_ext_remove_tool_internal` is the MCP retire seam. |
| `Makefile`, `tools/gen-exts.py` | P2 fix wave: the build list is `extensions/manifest.json` — every `c` entry is compiled/linked (not just `hello`) and the `rust` entries are built through cargo from the same manifest; `gen-exts.py` rejects a manifest name whose sanitized form is not a valid C identifier in every mode (`--list-c`/`--list-defines`/the registry writer). |
| `tests/mcp_test.c`, `tests/mcp.sh` | New live assertions: start-before-connect, pure snapshot, partial connect across pumps, pump-only re-sync, in-flight shutdown; all prior checks (incl. stubborn-child escalation and `mem_live`) kept. |
| `.agents/plans/EXTENSION-SPEC.md`, `.agents/docs/30-extensibility.md` | P2 records and deviations. |

---

### 5.3 P3 — hooks (fully delivered)

| file | change |
|---|---|
| `src/core/agent.c` | The P3 core dispatch: `want_override`/`want_observe` (quiet never gates an override); `input_hook`, `before_agent_start_hook`, `message_end_hook` (+ restricted `apply_message_patch`), `tool_result_hook` (content forms), `turn_end_hook`, `settle_hook` (+ `hook_apply_entries`, shared 32-continuation cap); `session_before_compact`/`session_compact`/`session_compact_failed` in `compact_now`; `before_provider_headers` flat patch (forbidden headers) and consuming `before_provider_request` (`REQ_BLOCKED`, `body_len` follows); agent-side veto consumer threads the rewritten `input` and `reason`; `message_end`/`tool_result` run before append/observer; `agent_settled` once per submit; `agent_end` payload enriched with `stop_reason`. |
| `src/app/project.c` + `src/app/project.h` (new) | Trust precedence + consumption (`project_trust` before the saved verdict; CLI wins; `undecided` falls back; `remember` persists; fail-closed on blocked/handled-without-result/malformed) and the `resources_discover` consumer (path gate: absolute, no `..`, config home always, cwd only when trusted; only `skillPaths` consumed). |
| `src/app/policy.c` + `src/app/policy.h` (new) | D23/D52 selection extracted from `main.c`: `--tools` > `default_tools` > "all"; the destructive default hides non-core `AGENTC_TOOL_DESTRUCTIVE` tools unless named (core `AGENTC_TOOL_CORE` stays eligible), one aggregate diagnostic; strict `--tools` defers `mcp__` names; `agentc_policy_recompose` is the recompose body. |
| `src/app/main.c` | New order: `register_defaults` → `register_linked` → `apply_config` → provisional `mcp_set_context(cwd,false)` → `load_all` → `resolve_trust` → final `mcp_set_context` → `apply_resources` → `builtin_context_set`; policy via `agentc_policy_apply`. |
| `src/ext/mcp.c` | D52 hint mapping: `readOnlyHint` wins, `destructiveHint` defaults true. |
| `tests/agent_loop_test.c` | P3 run-path tests (see spec §13): veto rewrite/block, input, before_agent_start, message_end, tool_result, turn_end, settle/cap, before_provider_headers/request, REQ_BLOCKED, observe bridge. |
| `tests/config_test.c`, `tests/resources_test.c`, `tests/compact_test.c`, `tests/policy_test.c` (new) | Trust precedence/remember/fail-closed; resources path gate + consumed skills; compact cancel/summary/failed; destructive default. |
| `include/agent.h`, `src/core/tools/registry.c` | `AGENTC_TOOL_CORE` internal marker; the core builtins carry it. |
| `.agents/plans/EXTENSION-SPEC.md`, `.agents/docs/30-extensibility.md`, `.agents/docs/20-core-agent.md` | P3 records: payload/result shapes, emit-site map, trust/resources semantics, test list. |

**Wave 4 (delivered).** `model_select`/`thinking_level_select` are emitted on a
real change by `agentc_agent_set_model`/`agentc_agent_set_thinking`
(`src/core/agent.c`), plus `model_select` from `agentc_mode_rebuild_agent` when
the effective provider/model changes. The app installs
`agentc_ext_set_model_sink`/`agentc_ext_set_thinking_sink` in
`agentc_mode_setup` (cleared in teardown): a same-provider model change goes
through the agent setter, a provider change rebuilds the agent, and
`set_thinking` accepts `off|low|medium|high` (unknown names logged and ignored).

### 5.4 R1–R3b — post-review fix waves (delivered)

The R1–R3b inventory with per-file anchors. Delivered and committed as
`c7e45bb` (docs follow in the next commit).

| file | change |
|---|---|
| `src/app/mode.c`, `src/app/mode.h`, `src/tui/tui.c`, `src/app/mode_rpc.c`, `src/core/agent.c` | one `agentc_mode_pump` duty installed for every mode as both the wire poll hook and the agent pump; the TUI chains through it and restores it after a submit; the RPC read-wait and TUI idle tick call it; `agentc_ext_pump` has a re-entrancy guard |
| `src/app/main.c` | `STARTUP_PUMP_BUDGET_MS 400` with early exit on `agentc_mcp_pending()`; auto prompt (`system_text = system_flag`, NULL = rebuild per turn); `--list-extensions`; `ext_log_summary`; the D52 `--help` note; `agentc_free(tools)` on the OAuth refusal path |
| `src/core/agent.c`, `include/agent.h` | `refresh_system()` per turn + per-run `system_override`; blocked `input` error event and negative submit; `message_end` `preserve_terminal` + tool_call validation; `turn_end.continue` on non-hard turns; direct registry `want_override`/`want_observe`; `agentc_ext_append_entry` |
| `src/ext/registry.c`, `include/agentc_ext.h`, `include/ext.h` | owner restore around every callback; `[ext:<name>]` log prefix; named failure/overrun diagnostics; 3-strike disable; unknown-point rejection; live-registration `wants`; emit depth 32; keyed `set_status`; `FIELD_OK`/`HOST_HAS` + `required_host_size`; `agentc_ext_describe`/`AgcExtInfo`/`agentc_ext_is_loaded`; `json_fields_apply`; bounded `load_all`/live shutdown |
| `src/ext/mcp.c` | bounded entries (256/server, 1024 total); over-long remote names skipped; description truncation; hash-suffixed 64-byte exposed names; frozen cap 256 where handed-out entries are never freed/reused (a full cap fails the server instead of recycling); `agentc_http_set_cancel`; `agentc_mcp_pending` readiness |
| `src/wire/http.c`, `include/wire.h` | `agentc_http_set_cancel` cooperative cancellation (`-ECANCELED`, never retried); ASCII `\uXXXX` header JSON |
| `src/tui/tui.c` | UI sink (`notify`; `set_title` falls back to a notice) |
| `Makefile`, `tools/gen-exts.py`, `.github/workflows/ci.yml` | `build/exts.mk` is a real make target with atomic writes and surfaced errors; `check-ext` under `make check`; CI runs plain `make`/`make ext-pipeline`/`make check` and `tests/ext.sh` under `nix develop` (dev-shell cargo, not apt) |
| `src/app/policy.c` | `select_tools` dedupe; MCP-disabled deferral diagnostic |

### 5.5 P4.1 — typed custom providers (delivered, independently verified, committed)

Design record: `.agents/plans/P4.1-PROVIDERS.md`; spec §11.1; decisions
D81–D96. The change set is committed as `f5de256` plus its docs commit
(`982fe3e`); the clean-tree gate run and independent audit are recorded in §8.

| file | change |
|---|---|
| `include/agentc_ext.h` | appended `add_provider` host slot + the provider constants and structs (`AgcExtProvider`, `AgcExtRequestView`/message/block/tool views, `AgcExtProviderAuth`, `AgcExtProviderModel`, `AgcExtStream`, `AgcExtStreamSink`, `AgcExtUsage`, `AgcExtWireEvent`) |
| `src/ext/registry.c` | `ProviderRec` pointer-vector registry; registration validation; request adapter (view + head sanitize + core auth); SSE adapter (`g_ext_sink`); owner retirement; shutdown free |
| `src/prov/provider.h`, `src/prov/provider.c` | appended row fields `is_ext`/`ext_rec`/`retired`/`auth_headers`; `agentc_provider_all`, `agentc_provider_open_ops`, `agentc_provider_retire_ext`; `registry_reset` back to the pristine baseline; the open-stream table tracks the row before `stream_open` |
| `src/prov/models.c`, `include/agent.h` | `AgcModel.flags` + `AGENTC_MODEL_STATIC`; `agentc_model_register_static`/`_clear_static`/`_is_static`; static and discovered rows share the 256-slot table |
| `src/core/discover.c` | discovery uses the row's `auth_headers` (NULL = the built-in Bearer default) |
| `src/core/config.c`, `include/config.h` | generic `providers.<id>.base_url`/`api_keys.<id>` (user scope, cap 32, id ≤64) |
| `src/app/main.c` | composition before onboarding and provider resolution; unknown-provider diagnostic; early-return shutdowns |
| `src/app/setup.c`, `src/app/setup.h` | `seed_presets`, `agentc_setup_provider_names`, row-based `--list-models`, extension entries in the onboarding menu |
| `extensions/fake_provider/fake_provider.c`, `extensions/manifest.json` | the reference provider example + manifest entry |
| `crates/agentc-ext/src/lib.rs` | Rust mirrors for the provider structs/constants and `Host::add_provider` |
| tests | `ext_test` (registry/adapters/validation/caps/static models + harness), `agent_loop_test` (round-trip/error/retry/cancel/unload/discovery/enumeration), `config_test` (generic config), `e2e.sh`/`ext.sh` (fake provider), goldens regenerated |

Run: `make ext-pipeline` / `tests/ext.sh` (the harness), `make check`
(`ext_test`/`agent_loop_test`/`config_test`), `tests/e2e.sh`.

### 5.6 P4.2 — skills/prompts/themes + MCP prompts/resources (delivered, verified; committed with the feature/docs commits)

Sub-waves: **P4.2a** (resource roots, one prompt registry, named themes,
`/skill:`, RPC `prompt_template`), **P4.2b1** (MCP capability negotiation +
prompts), **P4.2b2** (MCP resources as three generic tools). Everything below
is committed by the chair (feature `c134625`, docs `c8d18a7`); the clean-tree
gate run and independent verification are recorded in §8.

| file | change |
|---|---|
| `src/core/prompts.c`, `src/core/prompts.h` | **new** — the one invocable-prompt registry: pointer-stable records, `[a-z0-9._:-]{1,64}` names, later-wins re-registration, `list`/`has`/`expand`/`remove`/`clear_source`, the 512-registration cap and `agentc_prompts_load_file_templates` (source `"file"`, frontmatter `description`/`argument-hint`, `AGENTC_TEMPLATES_MAX` 64), plus the MCP expand callback registration used by `mcp.c` |
| `src/core/prompt.c`, `src/core/prompt.h` | removed the old per-root skill state; the resource-root accessors; `agentc_prompt_name_valid`; `agentc_skill_body_read` / `agentc_builtin_context_skill_body` for `/skill:<name>` |
| `src/core/resources.c` | process-lifetime deduped root stores per kind (8 roots; 32 paths/list); `agentc_templates_load_extra` + `argument-hint`; `agentc_themes_load` (64 per scan, sorted) and `agentc_theme_path` (validated name, roots → config dir); theme-stem validation |
| `src/app/project.c`, `src/app/project.h` | `resources_discover` consumes `skillPaths`, `promptPaths` **and** `themePaths` through the root stores; the 32-path cap; blocked stays fail-closed |
| `src/app/main.c` | loads the file templates after `apply_resources`; passes `cfg->theme` and the resolved trust verdict to `agentc_tui_run` |
| `src/app/mode_rpc.c` | the `prompt_template {name,args}` RPC command (expand → submit; unknown/missing name errors) |
| `src/app/policy.c`, `src/app/policy.h` | exact-name policy deferral for `mcp_list_resources` / `mcp_list_resource_templates` / `mcp_read_resource` (no broad `mcp_` prefix) |
| `src/base/limits.h` | the P4.2 caps (MCP prompt/resource tables and text, prompt/theme names and registry, resource roots/paths, skill submit) |
| `src/ext/mcp.c`, `src/ext/mcp_int.h` | capability parse and per-kind list sync (tools → prompts → resources → templates) with per-kind latches; the prompt registry commit (`mcp__<server>__<prompt>`, sync `prompts/get`, frozen pile); the resource/template tables and the three generic read-only tools published lazily at the first resource-capable sync; per-kind `list_changed` |
| `src/tui/theme.c`, `src/tui/theme.h` | named theme files with slot-by-slot precedence, the `COLORFGBG` `system` heuristic, project-root trust plumbing, `theme_apply_named` |
| `src/tui/tui.c`, `src/tui/tui.h` | `/theme <name>`, `/skill:<name>`, prompt-registry commands, the menu precedence (built-in > prompt > extension), `agentc_tui_run(agent, prompt, mode, theme_name, trusted)` |
| `include/config.h` | `AgcPromptTemplate.hint`, `AgcThemeEntry`, the theme loaders/path; `theme` is now honored |
| `include/mcp.h` | the capability/prompt/resource contract |
| tests | `tests/themes_test.c` (new); `tests/mcp_test.c` (capabilities/parsers/registries), `tests/mcp.sh` (live prompts/resources/gate variants) + `tests/mcp_mock.py`; `tests/tui_test.c` (`/theme`/`/skill:`/prompt commands); `tests/modes_test.c` (RPC `prompt_template`); `tests/resources_test.c` (roots/theme path/skill body/prompt registry); `tests/policy_test.c` (resource-tool deferral); regenerated goldens |

Run: `make check` (golden suites incl. `themes_test`), `./tests/mcp.sh`,
`./tests/e2e.sh` (incl. the pty TUI), `./tests/ext.sh`.

### 5.7 P4.3 — asynchronous extension tools (delivered, verified; committed with the feature/docs commits)

Design/delivery record: `.agents/plans/P4.3-ASYNC-TOOLS.md`; spec
§3.7/§4.7/§12–§16; decisions D110–D123. Sub-waves: **P4.3a** (ABI +
`AgcJob.cleanup` + adapter + retire discipline + Rust mirror +
`ext_test`/`jobs_test`), **P4.3b** (`async_demo` + `ext.sh` +
`agent_loop_test` + goldens); **P4.3c** (docs + independent verification +
commit) completed — this section is that record, and the chair committed.

| file | change |
|---|---|
| `include/agentc_ext.h` | appended `start`/`step`/`stop` to `AgcExtTool`; the five stop-reason constants and `AGENTC_EXT_TOOL_TIMEOUT_MAX_MS`; the contract comment |
| `src/core/tools/jobs.h`/`jobs.c` | appended `AgcJob.cleanup(self, job, reason)` + `AGENTC_JOB_STOP_*`; invoked exactly once (cleared first) on fatal/timeout/cancel/release before `job->priv` is freed |
| `src/ext/registry.c` | `ExtAsync` adapter in `job->priv` (call copy, cancel pointer, 32-byte token, extension state, LIFO list); `ext_tool_start`/`ext_tool_step`/`ext_tool_cleanup` with owner restore + signal window + per-step staging + `agentc_pump(0)`; registration gates (`run` wins, complete trio, `FIELD_OK`); async timeout clamp; `retired`/`free_pending` + `g_retired`; unload/remove retire with `stop(UNLOAD)`; shutdown defers a free past the last job |
| `crates/agentc-ext/src/lib.rs` | `ToolStart`/`ToolStep`/`ToolStop`, the reason/clamp constants and the three `AgcExtTool` fields (the `Tool` builder stays synchronous; an async Rust tool sets the raw fields) |
| `extensions/async_demo/async_demo.c` (new) + `extensions/manifest.json` | the reference example: `async_demo`/`async_fatal`/`async_slow`/`async_cancel`, the `async-demo-debug` test command |
| `tests/ext_test.c`/`tests/data/ext_test.expected`, `tests/jobs_test.c`/`tests/data/jobs_test.expected` | registration gates and runtime paths; the cleanup hook incl. release; harness checks for the linked example |
| `tests/agent_loop_test.c`/`tests/data/agent_loop_test.expected`, `tests/ext.sh` | the five-tool async fixture through the replay loop (ok/timeout/abort/interleave); the `async_*` harness checks |

Run: `make check` (goldens incl. `ext_test`/`jobs_test`/`agent_loop_test`),
`./tests/ext.sh` (linked `async_demo`), `./tests/e2e.sh`, `./tests/mcp.sh`,
`make windows`. The chair's gate run and independent verification are recorded
in §8.

Delivered deviations (D122/D123): shutdown defers a retired record's free past
the last live job (`free_pending`; `ext_async_unlink` releases it), and the
generic async output path applies the 50 KB `AGENTC_LIMIT_TOOL_BYTES` cap +
spill/truncation only — D-A4's 2000-line `AGENTC_LIMIT_TOOL_LINES` cap belongs
to `bash`/`read` and is not part of the extension-tool job path; `async_fatal`
fails on step index 2; `async_demo.fail_at` replaces the failing step line; the
`async-demo-debug` command is the test surface; and the extension's own
cancellation poll is covered by a direct adapter drive because the driver's
cancel check preempts it. Deferred: per-step watchdog, progress ABI, structured
content, per-extension quotas. Unload does not wait for live jobs; P4.5 adds the
dynamic half — the library handle is closed only after the last job unlinks
(never a join).

---

### 5.8 P4.4 — reserved points + completion (delivered, independently verified; committed with the feature/docs commits)

Design/delivery record: `.agents/plans/P4.4-COMPLETION.md`; spec
§4.1/§4.2/§4.7/§7/§8/§12–§16; decisions D125–D136. Sub-waves: **P4.4a**
(persistence-owner fix + `tool_call.terminate` + `session_before_switch`),
**P4.4b** (HTTP cancel + MCP idle drain/reconnect/status + tests-only
completions), **P4.4c** (docs + independent verification + commit) — this docs
pass is the record; the chair committed it as feature `54fbcd5` plus docs
`34af8a0`.

| file | change |
|---|---|
| `include/agent.h` | `agentc_agent_transport()` accessor; `AgcToolVetoDecision.terminate` (appended) |
| `include/agentc_ext.h` | chain-veto contract comment extended with `terminate`; `http_cancel` comment (in-flight interrupt, partial body) |
| `include/mcp.h` | P4.4b contract: idle stdin drain, reconnect/backoff (`AGENTC_MCP_RETRY_MS`), `mcp_servers_change` payload, FAILED non-pending |
| `src/base/limits.h` | `AGENTC_MCP_RETRY_BASE_MS` 1000, `AGENTC_MCP_RETRY_MAX_MS` 60000 |
| `src/core/agent.c` | all-terminate batch count/end (cancel wins, `turn_end.continue` cap); transport getter |
| `src/ext/registry.c` | `session_before_switch` point row; `terminate` parsed and gated on `block`; `volatile` HTTP `cancelled` flag handed to `agentc_http_set_cancel` |
| `src/ext/mcp.c`, `src/ext/mcp_int.h` | idle stdio drain (64 msgs/pump), reconnect/backoff (rearm to NEW), `mcp_status_name` + `mcp_servers_change` payload/emit sites, internal `agentc_mcp_retry_delay_ms` |
| `src/app/main.c` | front-end `PrintCtx` persistence removed; `mio.observer = NULL` selects the ctx-bound `mode_flush_observer`; compaction via `agentc_mode_note_compaction` |
| `src/app/mode.c`, `src/app/mode.h` | `agentc_mode_note_compaction`, `agentc_mode_emit_session_start`, `agentc_mode_rebind_session`; `agentc_mode_rebuild_agent` preserves an injected transport |
| `src/app/mode_rpc.c` | `session_before_switch` cancel gate in `rpc_new_session`; rebind + `session_start{reason:"new"}` after the swap |
| tests | `modes_test` (persist after set_model/new_session, switch cancel/allow, context follow), `agent_loop_test` (terminate all/mixed/continue/cancel), `ext_test` (switch caps/policy, terminate parse gate, failed-init rollback of commands/sections/status/pumps, `request_recompose`, priority ties, late `mcp__` recompose, in-flight HTTP cancel + baseline warm-ups), `mcp_test` (retry delays, FAILED rearm/recover, `mcp_servers_change` payload, `--live-idle`), `mcp.sh` (`--live-idle`; `--live-p2` with retry disabled), `mcp_mock.py` (idle timer + write lock); goldens regenerated |

Run: `make check`, `./tests/ext.sh`, `./tests/mcp.sh` (incl. the live idle
check), `./tests/e2e.sh`. **P4.4a/b were not built or run by this docs seat
(docs-only contract): the P4.4c gate run and independent audit are open, and no
verification is recorded until then.**

Delivered deviations (D134/D135): `agentc_agent_transport()` keeps an injected
transport alive across a `set_model` rebuild and the observer rebind is
unconditional; the MCP retry env overrides the base delay only;
`agentc_mcp_retry_delay_ms` is internal; `retry_count` resets only at record
creation; `--live-idle` and the mock idle timer are the idle-drain test surface;
`--live-p2` runs with `AGENTC_MCP_RETRY_MS=0`; `ext_test` warms the provider and
net-mock caches before its allocator baseline.

---

### 5.9 P4.5 — dynamic extension loading (delivered, committed; platform runtime gated on CI)

Design/delivery record: `.agents/plans/P4.5-DYNAMIC.md`; spec
§2/§3.5/§5/§9/§10/§12–§16; decisions D138–D147. Sub-waves: **P4.5a** (plat
loader surface + the `dynlib` scan/adopt/close policy + registry lifecycle +
the fake-loader golden suite) and **P4.5b** (real shared-object targets,
`tests/dyn.sh`, the Wine gate and the CI runtime jobs). This docs pass is the
record; the chair committed the P4.5 files together as feature `9f4b7db` plus
docs `9f3c12b` (§9.4).

| file | change |
|---|---|
| `include/plat.h` | `os_ext_open/sym/close/available/suffix` loader surface |
| `include/ext.h` | `agentc_ext_register_dynamic()`; `AgcExtInfo.dynamic` |
| `src/plat/linux/ext.c` | fail-closed stub (`-ENOSYS`/false/NULL, one clear message) |
| `src/plat/mac/ext.c` | `dlopen(RTLD_NOW\|RTLD_LOCAL)`/`dlsym`/`dlclose`, `.dylib` |
| `src/plat/mac/mac_internal.h` | `<dlfcn.h>` |
| `src/plat/win/ext.c` | `LoadLibraryExW` (DLL-dir + System32, fail closed), `GetProcAddress`/`FreeLibrary`, Win32 error text |
| `src/plat/win/win.h`, `src/win/kernel32.def`, `src/plat/win/rt.c` | imports (`LoadLibraryExW`, `GetProcAddress`, `FreeLibrary`), search flags, error-code constants, `win_errno` mappings |
| `src/ext/dynlib.c`, `src/ext/dynlib_int.h` | injectable `AgcExtDynLoader` seam; scan of `<config home>/extensions/` (suffix/stem/order/cap/filters) and `agentc_ext_dyn_path_ok` |
| `src/ext/registry.c` | adopt entry owner-tag/rollback; `load_all` re-entrant-unload guard; `DynRec` + idle-only exactly-once close discipline (deferred to job unlink/pump/shutdown, keep-mapped warning at shutdown); status-provider trampoline (Finding 1) |
| `src/app/main.c` | `agentc_ext_register_dynamic()` in the three composition sites; `(dynamic)` in `--list-extensions` |
| `tests/dyn_test.c`, `tests/data/dyn_test.expected` | 79-check golden suite with a fake loader (scan/register/load/unload/shutdown lifecycle) |
| `tests/dyn.sh` | the real-loader runtime gate (macOS `dlopen`, Windows/`--wine` `LoadLibraryExW`, plain-Linux build-only skip) |
| `Makefile` | `ext-dylib`, `win-ext-dylib` (hello.dll + nosym.dll), `dyn-check` |
| `.github/workflows/ci.yml` | `linux` dynamic gate (`ext-dylib` + `win-ext-dylib` + `dyn-check`), `macos-14` `tests/dyn.sh`, `windows-latest` `tests/dyn.sh --windows` |

Recorded verification and the CI-gated remainder are in §8; deviations are
D138–D147 and `.agents/plans/P4.5-DYNAMIC.md` (`register_dynamic` is void, the
path policy runs twice, Windows keeps the regular-file check, one warning
covers the shutdown sweep, the status trampoline and the loader-table
validation are the two verification fixes).

---

### 5.10 Post-P4 close-out (delivered in the working tree)

The close-out fixes the two known tickets recorded in §9.4 and the review nits,
with decisions D148–D151. Working-tree files:

| file | change |
|---|---|
| `src/ext/mcp.c` | `agentc_mcp_write_all` attempts the first `PIPE_BUF` chunk write-first and falls into the bounded poll path only on `MC_EAGAIN`/`MC_EINTR`; later chunks keep the poll path (D148) |
| `src/ext/registry.c` | `g_loading` depth across `load_all` rounds and nested loads; `agentc_ext_shutdown()` during load is a logged no-op; the re-entrant-unload record is skipped by the post-`init` `!e->used` re-check (D149) |
| `src/app/mode_rpc.c` | `agentc_mode_rpc_run` rebinds the persistence observer to the caller's `AgcModeCtx` before returning; the run's `rpc_event` pump stays frame-bound by design (D150) |
| `src/ext/dynlib.c` | `dyn_home_absolute()` refuses a relative config home with a clear warn and skips the scan (D151) |
| `include/plat.h` | the `os_ext_*` `err` buffer is documented as filled on failure only |
| `.agents/plans/PI-EXTENSION-MAP.md` | the stale “agentc target” column is deleted, with a pointer to D5 and the spec |
| tests | `ext_test` ignored-shutdown fixture (shutdown-during-load plus the re-entrant-unload discrimination), `dyn_test` `scan_relative_home`, `modes_test` `mode.observer.rc`/`mode.observer.persists` (frame-poison discrimination), and the `mcp_test` write-bounded drain; the three golden files are regenerated |

Recorded run: `nix develop -c make wine-check` → all 30/30 Windows golden
suites passed under Wine (the previously red `mcp_test write.small=0` is
green). Residual (D148): on Windows anonymous-pipe writes are blocking and
`os_poll` has no write-pipe branch, so a full request pipe can sleep past the
deadline after the first chunk and a request larger than one `PIPE_BUF` still
times out; on macOS the parent write end is blocking too (`os_pipe` leaves it
so), so the same first-chunk probe can sleep past the deadline on a full pipe.
The full fix belongs in `src/plat/win/sys.c` plus a non-blocking macOS parent
write end. The close-out tree is
uncommitted until the chair commits it (§9.4).

---

### 5.11 P5 tool engine (T1 grep + T2 find; delivered in the working tree)

`grep` and `find` keep their names, JSON schemas and normalized output; the
backend is now a process-wide choice resolved at `builtin-tools` init
(`--tools-engine external|internal`, `AGENTC_TOOLS_ENGINE`, `tools.engine`).
Chains: `grep` ripgrep → POSIX grep → built-in, `find` fd → POSIX find →
built-in, `ls` always in-process; the POSIX rungs are POSIX-only. Decisions
D152–D163. Working-tree files:

| file | change |
|---|---|
| `include/plat.h` | appended `os_which_in`/`os_which`: PATH probe, absolute result, `PATHEXT` on Windows, execute bit on POSIX, `0`/`-ENOENT`/`-ERANGE` (D156) |
| `src/plat/{linux,mac,win}/sys.c` | the three implementations of the probe |
| `include/config.h`, `src/core/config.c` | appended `AgcConfig.tools_engine` + the global getter/setter, `tools.engine` parsing and the `AGENTC_TOOLS_ENGINE` override (D153) |
| `src/app/main.c` | `--tools-engine` flag (validated, applied after config load) and the one startup line via `agentc_tool_engine_log()` |
| `src/core/tools/engine.h` (new) | core-internal contract: modes, backends, selection, shared arg views, pure `build_argv`, the two line normalizers, the test seam |
| `src/core/tools/engine.c` (new) | ordered descriptor table as the single source of truth (per row: binary, `posix_only`, `exit1_nomatch`, display, description, `build_argv`, `run`), resolution/logging/reset/selection over the table, argv mapping, normalization, and one shared `proc_spawn`/`proc_pump` reader for grep+find (spawn/read/checkpoint/group-kill/reap, empty env, capped stderr, output-byte caps) |
| `src/core/tools/grep.c`, `find.c` | `agentc_tool_grep_args`/`agentc_tool_find_args` factored out; the built-in run functions use them unchanged |
| `src/core/tools/registry.c` | `builtin-tools` init resolves the engine and substitutes per-tool `desc`/`run`; the `grep_desc`/`find_desc` statics moved into `engine.c` |
| `tests/engine_test.c` + golden (new) | 90 labels: parse, synthetic-PATH resolution, selection, per-backend descriptions, exact argv, normalizers, and the external rg/fd runs through the job driver (limit/cancel/timeout, POSIX-gated so the shared golden also passes under Wine) |
| `tests/e2e.sh`, `tests/mock_provider.py` | an external `grep` round-trip through a stub `rg` on `PATH`, asserting the tool result reaches the second request and the prompt names the effective backend; `--tools-engine internal` names the built-in engine instead |
| `.agents/docs/20-core-agent.md` §4.5, `.agents/docs/30-extensibility.md` | the engine documentation and the one-line note that extensions are unaffected |

Verification: `make check` green (31/31 golden programs including the new
`engine_test`), `./tests/ext.sh`, `./tests/mcp.sh`, `./tests/e2e.sh`,
`make windows`, `nix develop -c make dyn-check` and
`nix develop -c make wine-check` (31/31 under Wine) all green. The tree is
uncommitted until the chair commits it.

The robust R1 redesign (`decisions.md` ADR-5) preserves this public behavior; it
reshapes `engine.c` around the ordered table and the shared `proc_*` reader, adds
the output-byte caps and the per-call argument arenas, and removes the dead
engine API (`engine_test` is regenerated for the new seam).

R3 (`decisions.md` ADR-8) then replaces the per-call 1 MiB JSON arena with the
shared `AGENTC_LIMIT_TOOL_ARENA` block (16 KiB, grows on demand) for the argument
parsers and the rg line scratch, switches the POSIX `grep` rung from the GNU long
options to the portable `-i`/`-C`/`-m` short forms (`--include`/`--exclude-dir`
stay as the documented approximation), and makes an over-long pending grep line
stop with the byte marker.

---

## 6. Decisions log

Accepted decisions (D), the resolved open decisions (O). Rationale is included so
a future agent can revisit deliberately.

| id | decision | rationale | status |
|---|---|---|---|
| D1 | Break the plugin ABI once; one header `include/agentc_ext.h`, `AGENTC_EXT_ABI 1`; delete `agentc_plugin.h` | keeping v1/v2 would give four struct families and a third adapter layer; the ecosystem is two examples | accepted |
| D2 | One extension model; `builtin-tools` and `mcp` are ordinary default extensions | no privileged path; matches the status-line precedent | accepted |
| D3 | Structure typed, data JSON | contributions are C structs; hook payloads/results are JSON (house rule) | accepted |
| D4 | Two hook capabilities: `OBSERVE` and `OVERRIDE` | mirrors pi's two categories; veto/transform/inject all "return a result" | accepted |
| D5 | Hook-point names are pi's verbatim | compatibility and doc alignment; the `<subject>_<phase>` rule is exploratory only | accepted |
| D6 | Minimal features, complete seams | the thesis; pi 2.0 durable machinery is a service concern | accepted |
| D7 | Extension tools are synchronous (no `start`/`step` in the ABI) | async needed a job output/error channel the ABI lacked; no extension tool needs it | accepted |
| D8 | Drop `add_provider` from ABI 1 | the real provider ops need typed request/stream; a `void*` vtable cannot express it; P4 | accepted |
| D9 | Output sinks are opaque `out_write(void*, bytes, n)`; never expose `AgcBuf` | keeps the internal buffer out of the frozen ABI | accepted |
| D10 | Publish `AgcExtToolCall` as an append-only struct | a tool must be able to read args and cancellation | accepted |
| D11 | `on` returns a handle; `off`; owner-tagged registrations with rollback | v1 had no unregister (dangling callbacks on shutdown) | accepted |
| D12 | `emit` takes an `AgcExtResult` out-param | extension-published points can be decisions, not only notifications | accepted |
| D13 | Override points are fail-closed on handler error/overrun; observe points fail-open and may be disabled | v1's permanent-disable-on-overrun was fail-open for permission gates | accepted |
| D14 | Recompose applies only after `agentc_tool_jobs_run`, at the exact turn boundary | `agentc_pump` runs mid-job (`bash.c`); swapping the table there frees live `AgcJob.tool` pointers | accepted |
| D15 | MCP `start`/`pump` split; `agentc_mcp_tools` a pure snapshot; re-sync in the pump | otherwise the registry collect still blocks on `sync_pending` | accepted |
| D16 | Load extensions before trust resolution and consume `project_trust` | the hook fires after resolve and its result is discarded — dead code | delivered (P3, D55) |
| D17 | Extension-registered destructive tools default hidden, not auto-selected | an extension must not silently gain destructive default access | delivered (P3, D52) |
| D18 | Custom providers and async extension tools deferred to P4 | keeps ABI 1 minimal and correct | accepted |
| D19 | `before_provider_headers` runs on the already-extracted header block, post-assembly/pre-send | headers/body are already separate; pre-assembly would forbid modifying provider headers | delivered (P3, D61) |
| O1 | `--system` semantics with sections | **resolved: hard-replace.** Extension sections are skipped; matches pi's `forceSystemPrompt`. `--append-system-prompt` is not added. | accepted |
| O2 | Composition order | **resolved: defaults-first** (`builtin-tools`, `mcp`, then linked by registration order). MCP is an ordinary extension; no existing golden encoded the order, so none needed regenerating. | accepted |
| D20 | one-word convention: extension/ext only | "plugin" removed from live code, headers, symbols, dirs, tests, the Rust crate and docs | accepted |
| D21 | Default extensions get exactly one privilege | they compile inside core, so they may include `src/ext/registry_int.h` and use the internal bus (async internal `AgcTool`, direct emit, pump hooks). Everything else is the public `AgcExtHost`. | accepted |
| D22 | Extension ordering in the ABI | `AgcExt.order` (int32, lower first, ties by registration order) orders `init` and therefore contributions; per-contribution priorities stay on section/status/hook | accepted |
| D23 | Recompose filter lives in the app, not the registry | the spec said "the core applies dirty" but did not say how the CLI `--tools`/`default_tools` policy survives a swap; added app-installed `AgcRecomposeFn` (`agentc_agent_set_recompose`) that re-runs the selection and installs it | accepted (P2) |
| D24 | Periodic default-extension work via `agentc_ext_add_pump` | keeps MCP out of `registry.c` while letting a default extension hook the main-loop pump; owner-tagged and cleared on unload | accepted (P2) |
| D25 | MCP stdio is a true state machine; HTTP is one bounded request per pump step | `agentc_http_run` is not resumable (no wire-layer stepping API); recorded as a spec deviation | accepted (P2) |
| D26 | `agentc_mcp_load` kept as a synchronous test helper | avoids churn in the offline `mcp_test`/`mcp.sh` harness; the app path is `start` + the registry pump | accepted (P2) |
| D27 | Pump callbacks run with their owner active; `agentc_ext_shutdown` releases all tool records | pump-added tools were owner `0` and leaked/did not roll back; this makes them owner-tagged and cleanup complete | accepted (P2) |
| D28 | `MERGE_REPLACE` honored; only override points merge a result | the point table declared the algebra but emit merged every result as fields; replace points (e.g. `project_trust`) must take the result wholesale | accepted (P1-completion) |
| D29 | an `OBSERVE` handler returning `1` is rejected with an error log | the `1` return requires `OVERRIDE` (§4.4); silently treating it as handled made observe points stop the chain | accepted (P1-completion) |
| D30 | dispatch walks a true subscriber snapshot | `off()` during the walk zeroed a record the current emit still held a pointer to; registration during dispatch stays visible only to the next emit | accepted (P1-completion) |
| D31 | dirty scope: public `add_tool` + `request_recompose` set dirty; commands/sections are read live | late tools need a turn-boundary recompose; commands/sections have no snapshot to rebuild | accepted (P1-completion) |
| D32 | unload/shutdown remove an owner's registrations before `shutdown()`; shutdown ends with `agentc_status_reset()` | `shutdown()` must not see dangling registrations; the status table must not outlive the registry | accepted (P1-completion) |
| D33 | `set_status` is an auto-clearing owner-tagged status segment; `set_model`/`set_thinking` stay optional front-end sinks | folds push status into the one status path; no front end installs model/thinking in P1, so an unset `set_model` fails with `-ENOSYS` instead of pretending success (P3 installs them in `agentc_mode_setup`, D66) | accepted (P1-completion; sinks delivered P3) |
| D34 | spec/doc corrections from the P1-completion review | §5 API block matched to `include/ext.h`; §6.1 internal add; §0/§16 O2 pump order; §4.7 marked target; §13 test claims; docs/30 `#ifdef`+NULL table, hook/status rules; docs/20 §8 removed a nonexistent flag | accepted (P1-completion) |
| D35 | Tool/registry records are pointer-stable | growing `g_tools` must not invalidate the `ToolRec*` stored in an already handed-out `AgcTool.ud`; the storage-stability regression guards it | accepted (P1-hardening) |
| D36 | Owner rollback covers async work | deferred callbacks and pending extension HTTP requests are owner-tagged and dropped with the owner; `agentc_ext_shutdown` resets the defer ring under its lock | accepted (P1-hardening) |
| D37 | Veto decision contract | `AgcToolVetoFn` is an `AgcToolVetoDecision{block,reason,args_json}` callback; the adapter consumes `reason` and a rewritten `input`; `1`/`{"block":true}`/failure block; input-only rewrites return `0` (agent-side consumer delivered in P3, D53) | accepted (P1-hardening; consumer delivered P3) |
| D38 | `fields` result is a patch | keys replace, JSON `null` deletes, the accumulator preserves delete markers, the next handler gets the effective payload; consumers use `agentc_ext_merge_fields` | accepted (P1-hardening) |
| D39 | `before_provider_headers` result is a flat header patch | `{ "<Header>": value\|null }` merged by the consumer so one `null` deletes one header; the generic `fields` merge stays shallow | accepted (P1-hardening) |
| D40 | Quiet never suppresses OVERRIDE points | `agentc_events_set_quiet` gates front-end delivery only; override emit must not use the quiet-gated `want()` (bridge/veto guards fixed in the P1-hardening wave; agent-side `want_override`/`want_observe` split delivered in P3, D54) | accepted (P1-hardening; split delivered P3) |
| D41 | Chain `rc == 1` does not stop the chain | it records `handled`; only `first`/`chain-veto` short-circuit; the `agentc_ext.h` handler comment was corrected | accepted (P1-hardening) |
| D42 | `agentc_ext_register` validates the descriptor | `abi_version`/`struct_size` (and name) are checked before the extension enters the pending set; an ABI register test covers it | accepted (P1-hardening) |
| D43 | `agentc_ext_remove_tool_internal(name)` | retires a tool by name, marks dirty, never frees handed-out data; MCP re-sync uses it to retire/replace entries (delivered in the P2 fix wave) | accepted → delivered |
| D44 | MCP pump budget: one server per call | `agentc_mcp_pump` advances exactly one server per call, round-robin (delivered in the P2 fix wave) | accepted → delivered |
| D45 | `extensions.disabled` covers defaults + linked | config is applied after `agentc_ext_register_linked()` and before `load_all`, so a linked extension can be disabled by name (delivered in the P2 fix wave; see D50) | accepted → delivered |
| D46 | MCP retiral/replacement reaches the registry | `mcp_sync_commit` withdraws a retired or replaced entry with `agentc_ext_remove_tool_internal(name)`; `mcp_publish_tools` marks an entry registered only on a successful `agentc_ext_add_tool_internal` (a duplicate/failed publish is logged once). This completes the §7 "pure snapshot + pump-owned re-sync" promise: a changed description shows in the next collected table, a removed tool is no longer advertised | accepted (P2 fix wave) |
| D47 | MCP pump: exactly one server per call, round-robin | a pump step is bounded to one server slice, so one slow/unreachable server cannot consume the budget or starve the others; implements D44 | accepted (P2 fix wave) |
| D48 | `list_changed` during an in-flight sync is latched | the notification sets a pending flag that the commit preserves, so the follow-up sync runs after the current commit instead of being cleared by it — a change during sync is never lost | accepted (P2 fix wave) |
| D49 | Strict `--tools` defers `mcp__` names | a name beginning `mcp__` absent at startup is skipped, then selected by `agentc_policy_recompose` when the server connects and the app recomposes at the turn boundary; a missing non-MCP name still exits 2, so startup stays strict for typos | accepted (P2 fix wave) |
| D50 | `extensions.disabled` order verified for linked names | implements D45: the app applies the config **after** `agentc_ext_register_linked()` and the registry removes adopted linked extensions by name, not only the defaults | accepted (P2 fix wave; implements D45) |
| D51 | Build scales with the manifest | the Makefile compiles/links every `c` entry in `extensions/manifest.json` (not just the `hello` example) and builds the `rust` entries from the same manifest, so more than one of either kind links without a Makefile edit; `gen-exts.py` rejects any manifest name whose sanitized form is not a valid C identifier, in every mode | accepted (P2 fix wave) |
| D52 | MCP `destructiveHint` default | **resolved (P3):** `readOnlyHint:true` wins; otherwise `destructiveHint` defaults to **true** (the MCP schema default), so an annotation-free tool is destructive and the §10 default hides it unless named. Core builtins carry `AGENTC_TOOL_CORE` and stay eligible; one aggregate diagnostic per selection. See D63 | accepted (P3, resolved) |
| D53 | P3 dispatch split: result-consuming points direct, observe points bridged | `message_end`/`tool_result`/`turn_end`/`input`/`before_agent_start`/provider/compact/trust/resources points are dispatched directly with `agentc_ext_emit` (never quiet-gated); observe points that already have a typed front-end event stay on the typed hub bridge. The new `want_override` (registry only) and `want_observe` (registry + quiet gate) guards keep `agentc_ext_wants` as the fast path | accepted (P3) |
| D54 | Quiet orthogonality in the agent | `agentc_events_set_quiet` gates front-end delivery only; every declared OVERRIDE emit site dispatches regardless, and observe points with no typed event (`session_compact*`, `agent_settled`) use `want_observe`; `turn_end` is no longer mapped by the typed bridge (the direct override owns it) | accepted (P3; closes D40) |
| D55 | Trust precedence | explicit `--approve`/`--no-approve` wins (hook not consulted); else the first `project_trust` decision (`yes`/`no` decide, `undecided` falls back to `agentc_trust_resolve`); `remember:true` persists via `agentc_trust_save`; blocked/handled-without-decision/malformed is fail-closed (untrusted, no fallback) | accepted (P3; implements D16) |
| D56 | Project-config limitation | the project config is loaded before extensions, so a project that becomes trusted only via `project_trust` cannot retroactively apply `<cwd>/.agentc/config.jsonc` this run; a warning says so and the verdict applies to context/skills/prompts/MCP immediately | accepted (P3, documented spec §8) |
| D57 | MCP start deferral | `agentc_mcp_set_context(cwd,false)` is set before `load_all` so no server can start before trust; after `resolve_trust` the final context is set. No registry pump runs in between, so the extension creates its servers on the first pump with the final context | accepted (P3) |
| D58 | Settle continuation semantics + cap | `entries` append through the host entry sink (malformed skipped); `continue` re-enters the turn loop (only on a clean run for `agent_before_settle`, only on a non-hard turn for `turn_end`); `turn_end` and `agent_before_settle` share a 32-continuation cap per submit | accepted (P3) |
| D59 | `message_end` restricted shape | the replacement `message` may set role (absent/assistant), a `content` array of text/thinking/tool_call blocks, `stop_reason` and `error`; `usage`/`ts` are never replaceable; malformed content leaves the original untouched | accepted (P3) |
| D60 | `tool_result` content forms | a replacement `content` may be a string, a `[{type:"text",text}]` array (newline-joined) or null/`[]` (empty); other shapes are ignored and the raw result kept; `is_error` is overridable | accepted (P3) |
| D61 | Header patch rules | flat `{"<Header>": value\|null}`; values pass the CRLF→space sanitizer; keys pass `hdr_key_ok`; `Content-Length`/`Transfer-Encoding`/`Host` are dropped (case-insensitive); duplicates: payload keeps the last occurrence, the patch replaces every occurrence in place/drops on null, unmatched keys append in patch order, last duplicate patch key wins; a malformed patch keeps the original block | accepted (P3) |
| D62 | `REQ_BLOCKED` failure semantics | a blocked/failed `before_provider_headers`/`before_provider_request` is terminal: error `request blocked by extension`, one error path, no retry, no tools, no transport call; a blocked compaction summary request fails compaction | accepted (P3) |
| D63 | `resources_discover` scope | emitted at startup after trust; paths must be absolute, without any `..` segment, under the config home (always) or the project cwd (trusted only); only `skillPaths` is consumed; `promptPaths`/`themePaths` are validated and logged as not consumed | accepted (P3; P4.2 consumes all three lists, D108) |
| D64 | `session_before_compact` payload | `{reason, first_kept, messages}` (no `entries`/`instructions`); result `{cancel?, summary?}`: a non-empty summary skips the provider request and splices verbatim, cancel cancels, handled-without-summary cancels; "nothing to do" and a cancelled summary emit no `session_compact_failed` | accepted (P3) |
| D65 | Policy/project module extraction | `main.c`'s D23/D52 selection moved to `src/app/policy.c` and the trust/resources ordering to `src/app/project.c`, so startup and the turn-boundary recompose share one implementation and `main.c` keeps only composition | accepted (P3) |
| D66 | App model/thinking sinks | wave 4 installs `agentc_ext_set_model_sink`/`agentc_ext_set_thinking_sink` in `agentc_mode_setup` (cleared in teardown) so `host->set_model`/`set_thinking` reach the mode context; `set_thinking` accepts `off\|low\|medium\|high`. `model_select`/`thinking_level_select` emit on a real change from the agent setters and from `agentc_mode_rebuild_agent` when the effective pair changes. A provider switch from the sink is refused with `-ENOSYS`: a rebuild there could free the agent executing a tool or one a front end still holds; provider switching stays exclusive to the RPC command's idle rebuild | delivered (P3 wave 4) |
| D67 | One pump duty for every mode | `agentc_mode_setup` installs `agentc_mode_pump` as both the wire poll hook and the agent pump; the TUI chains through it instead of competing, and the RPC read-wait/idle tick call it. `agentc_ext_pump` keeps a re-entrancy guard so a pump-within-a-pump returns immediately | accepted (R1) |
| D68 | Startup pump is a 400 ms readiness budget | the app pumps after trust resolution; the first call creates the MCP records, then the loop exits as soon as `agentc_mcp_pending()` is false or 400 ms pass. `agentc_mcp_server_count()` is the configured-server count (terminal records stay counted), not a readiness count | accepted (R1/R3) |
| D69 | Auto prompt rebuild per turn | with no explicit `--system` the agent rebuilds the prompt from the live tool table before every turn (`refresh_system()`), so a turn-boundary recompose reaches the next request; `--system` and a per-run `before_agent_start` `systemPrompt` override stay fixed, and the effective prompt is re-published to the extension context | accepted (R1) |
| D70 | Owner restore + named logs | every hook/tool/command/section/pump/defer/HTTP callback runs with its owning extension active, so `set_status`/`defer`/`add_tool` attribute correctly; `host->log` is prefixed `[ext:<name>] `, and failure/overrun lines name the extension | accepted (R2) |
| D71 | 3-strike overrun containment | a per-handler consecutive-overrun counter resets on a clean call; three consecutive overruns disable the handler process-wide. The overrunning occurrence is fail-closed (block) on override points and fail-open (ignored) on observe points. Supersedes the P1 rule that override handlers are never disabled | accepted (R2) |
| D72 | Unknown points rejected; emit recursion capped | `on` accepts only spec §4.1 names (level-3 log otherwise); nested emits stop at depth 32, where an override point answers blocked and an observe point answers empty | accepted (R2) |
| D73 | ABI growth gates | appended contribution fields are read with `AGENTC_EXT_FIELD_OK`, appended host services with `AGENTC_EXT_HOST_HAS`; `AgcExt.required_host_size` declares the minimum host size and is refused above `sizeof(AgcExtHost)` | accepted (R2) |
| D74 | Terminal-preserving message_end; non-hard turn continuation; blocked input is visible | `message_end` may replace content on a failed/aborted turn but not `stop_reason`/`error` (and it validates tool_call entries); `turn_end.continue` is honored on every non-hard turn; a blocked/failed `input` sets `last_error`, emits `AGENTC_EV_ERROR` and returns -1 from submit | accepted (R2) |
| D75 | `set_status` keyed by (owner, key) | two extensions may use the same key; 8 live keys process-wide, a ninth logged and ignored; owner teardown clears its keys | accepted (R2) |
| D76 | MCP memory bounds | 256 tools/server and 1024 total; over-long remote names are skipped (never truncated); descriptions truncate at 8 KiB; exposed names are hash-deduped inside 64 bytes; retired-but-handed-out entries are capped at 256 and never freed or reused (a stale `AgcTool.ud` can reference one until the next recompose); the cap fails the server with a clear log instead of recycling | accepted (R3; hardened by F1) |
| D77 | Extension observability | `agentc_ext_describe`/`AgcExtInfo` + `agentc_ext_is_loaded` back `--list-extensions` and the post-`load_all` summary; the `--help` notes the D52 default; a deferred `mcp__` name is diagnosed differently when MCP is disabled (no false connection promise) | accepted (R3) |
| D78 | TUI UI sink | the TUI installs `AgcExtUiSink` (`notify` appends a chat notice); `set_title` logs and falls back to a notice because the terminal layer has no title op; print/json/rpc leave the sink unset and keep the log fallback | accepted (R3) |
| D79 | Build fragment is a real target | `build/exts.mk` is regenerated by a make rule (no parse-time `$(shell)` side effect), written atomically by `tools/gen-exts.py`, is a prerequisite of every object, and a validation error fails the build; `make check` includes `check-ext`; CI's Rust examples build with the nix dev-shell cargo | accepted (R3) |
| D80 | Bridge retired; core helpers unified | `AgcWantsFn`/`AgcNotifyFn`/`agentc_agent_set_ext_bridge` are gone: the core calls the registry directly and uses `agentc_ext_append_entry` for entries. `json_fields_apply(base, over, keep_nulls)` is the one fields helper (`agentc_ext_merge_fields` = keep_nulls false). `load_all` runs bounded rounds and shutdown walks the live record list to completion | accepted (R3) |
| D81 | P4.1 request view crosses the ABI as a host-materialized struct (D-P1) | `AgcExtRequestView` + message/block/tool arrays instead of opaque internals or accessor host slots; one `agentc_alloc` per build attempt, freed after the call. `stream_open(st)` has no row parameter, so `agentc_stream_state_init` tracks the row before `stream_open` and `agentc_provider_open_ops(st)` recovers it (`src/prov/provider.c:333–361`) | delivered (P4.1a) |
| D82 | P4.1 two-sink request contract (D-P2) | `build_request` writes head and body into `head_out`/`body_out` via `host->out_write`; the core inserts the blank line, sanitizes both (CRLF→space, token keys, forbidden trio, the declared auth header), adds the auth line and `Content-Length`, and joins the parts | delivered (P4.1a) |
| D83 | P4.1 composition above resolution (D-P3) | defaults → linked → apply_config → load_all runs before provider resolution **and** before first-run onboarding; the `agentc setup` subcommand composes with MCP skipped so the menu and discovery see extension providers; `--list-models`/`--list-extensions` early returns shut the registry down (deviation D93) | delivered (P4.1b) |
| D84 | P4.1 provider rows frozen until shutdown (D-P4) | `ProviderRec` pointer vector, owner-tagged; unload retires (`alive=false`, `ops.retired=true`) instead of freeing; lookups skip retired, a held handle still resolves for a clean `-ENOSYS`/`provider unloaded`; shutdown = `retire_ext` → `registry_reset` → free records; no refcount, and a retired row's name stays reserved (re-registration refused with `-EEXIST`) so a held handle cannot be redirected. `registry_reset` now returns to the pristine baseline (frees the row vector and open-stream table, lazily re-seeds the builtins) | delivered (P4.1a + fix) |
| D85 | P4.1 duplicates rejected, cap 64 (D-P5) | duplicate names against builtins, materialized presets and live extension rows are logged and ignored; `AGENTC_EXT_MAX_PROVIDERS` 64; a failed `add_provider` never fails `init` | delivered (P4.1a) |
| D86 | P4.1 core-applied auth (D-P6) | the core resolves the key and applies it per `auth.kind` (NONE/BEARER/HEADER); the key is never in the view and never reaches the extension; BEARER fills `Authorization`/`"Bearer "` defaults; HEADER requires a token-safe header and prepends the prefix verbatim; QUERY is reserved/rejected | delivered (P4.1a) |
| D87 | P4.1 SSE only (D-P7) | the extension receives parsed `AgcExtWireEvent`s and calls the typed `AgcExtStreamSink`; arbitrary framing (`stream_bytes`) stays deferred | delivered (P4.1a) |
| D88 | P4.1 mandatory vs optional callbacks (D-P8) | `build_request`/`stream_event` are mandatory; `stream_open`/`stream_finish`/`stream_close` NULL = stateless/default; `st->ud` is extension state allocated in open and freed in close | delivered (P4.1a) |
| D89 | P4.1 static models (D-P9) | registered at `add_provider`, marked `AGENTC_MODEL_STATIC`, survive `agentc_model_clear_dynamic`; the new `agentc_model_clear_static` drops them when the record is freed; static and discovered rows share the 256-slot table (one-time full log); a dynamic update never clears the bit | delivered (P4.1a; deviation: `clear_static`) |
| D90 | P4.1 generic config (D-P10) | `providers.<id>.base_url`/`api_keys.<id>` in user scope only, ≤32 each, id ≤64, named presets consulted first; project scope stays ignored | delivered (P4.1b) |
| D91 | P4.1 discovery styles + per-row auth (D-P11) | `NONE/DEFAULT/ANTHROPIC/OLLAMA` reused; `CUSTOM` deferred; discovery calls the row's `auth_headers`; an empty override is authoritative (AUTH_NONE sends no auth line) and only rows with no hook fall back to the builtin `authorization: Bearer`, so a HEADER-auth provider can list models. The delivered app enumerates rows through the new count-first `agentc_provider_all` (name+api dedup) instead of a hardcoded name list | delivered (P4.1a/b) |
| D92 | P4.1 custom rows use `api = name` (D-P12) | sharing an existing wire api is deferred; so are cost rates, per-model thinking config, multimodal, OAuth providers and `providers.<id>.headers` | delivered (P4.1b) |
| D93 | P4.1b onboarding composition order | composition runs before onboarding too (and in the `agentc setup` subcommand, MCP skipped); D-P3 named only the provider-resolution move | delivered (P4.1b deviation) |
| D94 | P4.1b preset-seeding helper | the lazily materialized OpenAI-compatible preset rows are seeded (`seed_presets`) before `agentc_provider_all`, because the registry has no preset-table accessor; `agentc_setup_list_models`/`agentc_setup_provider_names` share it | delivered (P4.1b deviation) |
| D95 | P4.1b e2e stock-binary skip | `tests/e2e.sh` probes `--provider fake --list-models --offline` only when the binary links the manifest entry; the stock binary prints an explicit skip; the real assertion runs on `build/ext_harness` | delivered (P4.1b deviation) |
| D96 | P4.1b harness coverage | the linked `fake_provider` assertions run in `ext_test`'s `--harness` mode (`tests/ext.sh`), not the golden fixture, which covers the `provfix` row; the loop paths are in `agent_loop_test`, the generic config in `config_test` | delivered (P4.1b deviation) |
| D97 | P4.2 has no ABI surface | prompts/themes/skills and MCP content are core/app features: `AgcExt` gains no `add_prompt`/`add_theme`/`add_resource` slot; the public route for an extension stays `resources_discover` roots and the internal `agentc_prompts_register` API is core-facing (file templates + MCP). Keeps ABI 1 frozen while the content layer grows | delivered (P4.2a) |
| D98 | Discovery is startup-only | the `resources_discover` roots are collected once (`main.c` after `apply_resources`) and file prompt templates load once right after; skill roots scan on each context render and theme names resolve on demand, but there is no root rescan or filesystem subscription, so a new root needs a restart. `resources_discover` remains one startup emit | delivered (P4.2a) |
| D99 | TUI command precedence | a built-in slash command shadows a same-named prompt, a live prompt shadows a same-named extension command, and `/skill:` is special-cased before the registry lookup; dispatch and the menu share the rule, so the menu cannot advertise an unreachable entry | delivered (P4.2a) |
| D100 | Theme precedence and roots | base = built-in name → `<config>/theme.jsonc` `base` → named file `base`; slots = config file then named file (the most specific file wins slot by slot). Name resolution: `resources_discover` theme roots (most specific first) → trusted project `.agentc/themes` → config `themes/`. The config `theme` key is the startup default (`system`) | delivered (P4.2a) |
| D101 | `agentc_tui_run` takes theme + trust | the app threads `cfg->theme` and its already-resolved project-trust verdict; the TUI does not re-derive trust, so a project trusted only via `--approve` or a one-shot `project_trust` hook can still resolve its themes | delivered (P4.2a) |
| D102 | `system` theme uses COLORFGBG only | last numeric field; 0–6 and 8 dark, 7 and 9–15 light; absent/unparseable → dark. The OSC-11 query is planned, not implemented | delivered (P4.2a) |
| D103 | MCP prompts are registry commands | a committed `prompts/list` registers `mcp__<server>__<prompt>` in the one prompt registry (source `"mcp"`), invocable like a file template from the TUI and RPC; arguments are parsed against the listed prompt arguments (≤16; required enforced, unknown/extra logged and ignored) | delivered (P4.2b1) |
| D104 | MCP prompt invocation is synchronous | one bounded `prompts/get` (`MCP_PROMPT_TIMEOUT_MS` 10 s) joins `type:"text"` blocks with `\n\n`, skips non-text with a log, caps the text at 64 KiB and fails over the cap instead of truncating; a slow prompt blocks the caller | delivered (P4.2b1) |
| D105 | MCP resources are three lazily published read-only tools | `mcp_list_resources`/`mcp_list_resource_templates` page the local tables as JSON (`cursor` = offset, 64 KiB/page) and `mcp_read_resource {server,uri}` does one bounded sync `resources/read`; published once at the first resource-capable list sync (marks dirty → next-turn recompose) and never removed | delivered (P4.2b2) |
| D106 | Resource-tool policy allowlist | the `--tools`/`default_tools` selection (`agentc_policy_apply`) defers the three exact resource-tool names like a not-yet-connected `mcp__` name; a `mcp_`-prefix typo is still a startup exit 2 | delivered (P4.2b2) |
| D107 | Per-kind MCP sync is capability-gated | `result.capabilities` chooses the fetched lists; missing/malformed/empty → tools-only; each advertised kind syncs in order (tools → prompts → resources → templates) with its own pending/again latch, cursor and page cap; `list_changed` is ignored when the capability is not advertised and latched during an in-flight sync | delivered (P4.2b1/b2) |
| D108 | Text-only resources + name validation | a `blob` entry rejects the read (`binary resource not supported`); over-long uris/names are skipped/truncated per the limits table; prompt/template names `[a-z0-9._:-]{1,64}` and theme stems `[A-Za-z0-9._-]{1,64}` (no `.`/`..`); 64 templates/themes per scan, 512 prompt registrations, 8 roots/kind, 32 paths/list, 256 KiB `/skill:` submit | delivered (P4.2a/b2) |
| D109 | P4.2 verification fixes | per-list path cap checked before validation (one excess log); MCP prompt argument descriptions capped at `MCP_MAX_PROMPT_DESC`; a ready-server MCP sync timeout re-arms `sync_pending` instead of abandoning the refresh (overlapping-exchange race); `skill:`-prefixed prompt/command names are reserved (hidden, explicit notice); `COLORFGBG` parsing is bounded (no signed overflow) | accepted (P4.2 verification) |
| D110 | P4.3 append `start`/`step`/`stop` to `AgcExtTool` (D-A1) | one appended trio crosses the ABI without a new contribution struct or host slot; `run` non-NULL stays synchronous and the trio is read behind `AGENTC_EXT_FIELD_OK`, so older/smaller contributions are unaffected | delivered (P4.3a) |
| D111 | P4.3 mandatory `stop` with five reasons (D-A2) | `stop(state, reason)` runs exactly once for every `start >= 0`; `FINISHED`/`ERROR`/`TIMEOUT`/`CANCELLED`/`UNLOAD`; no separate cancel callback — cancellation is polled through `is_cancelled` and delivered as `stop(CANCELLED)`; `FINISHED` means no more steps and `*is_error` is the result | delivered (P4.3a) |
| D112 | P4.3 extension-owned state (D-A3) | `void *state` is `host->alloc` in `start` and `host->free` in `stop`; the host frees only its adapter record | delivered (P4.3a) |
| D113 | P4.3 per-step output staging (D-A4) | `start`/`step` output goes into a per-step buffer appended with `agentc_tool_append_output`, so it obeys the same inline byte cap + spill and display cap as a sync result | delivered (P4.3a) |
| D114 | P4.3 `AgcJob.cleanup` (D-A5) | the driver's appended hook runs exactly once on fatal/timeout/cancel/release before `job->priv` is freed (cleared before calling); the adapter maps its reason to the tool `stop` reason | delivered (P4.3a) |
| D115 | P4.3 retire-not-free + `stop(UNLOAD)` (D-A6) | a record with live jobs is unlinked but kept (`tool.ud` is live); retiral stops each job `UNLOAD`; the record is freed at shutdown; a retired name is reusable | delivered (P4.3a; deviation D122 for the post-shutdown free) |
| D116 | P4.3 `run` wins (D-A7) | with both declared, `run` executes and the trio is ignored with a level-1 log (ABI compatibility by precedence) | delivered (P4.3a) |
| D117 | P4.3 signal window (D-A8) | one 32-byte token per job; `g_signal`/`g_signal_cancel` are set around every `start`/`step`/`stop`, so `is_cancelled(call->signal_token)` is valid from start through stop | delivered (P4.3a) |
| D118 | P4.3 pump per running step (D-A9) | the adapter calls `agentc_pump(0)` after each running step with the extension off the stack, so `defer`/`http_request` progress and a deferred unload can stop the job | delivered (P4.3a) |
| D119 | P4.3 async tools are not implicitly sequential (D-A10) | the batch driver's async fan-out/step interleave is unchanged (`a0b0a1b1`); `AGENTC_TOOL_SEQUENTIAL` still applies when declared | delivered (P4.3b) |
| D120 | P4.3 Rust mirror (D-A11) | the crate gains `ToolStart`/`ToolStep`/`ToolStop`, the constants and the three fields; the sync `Tool` builder is unchanged, so an async Rust tool builds the raw struct | delivered (P4.3a) |
| D121 | P4.3 async timeout clamp (OD-3) | async `timeout_ms` clamps to `AGENTC_EXT_TOOL_TIMEOUT_MAX_MS` (1 800 000 ms) with a level-2 log; a negative value normalizes to 0 (the driver's 120 s default); sync is unchanged | delivered (P4.3a) |
| D122 | P4.3a delivered deviations | shutdown defers the free: `agentc_ext_shutdown` cannot free a retired record while a live job still holds `tool.ud`, so it sets `free_pending` and the last job's unlink releases the record (`ext_async_unlink`); a retired record with no live jobs is freed during shutdown as D-A6 specified. Also: the generic async output path applies the 50 KB `AGENTC_LIMIT_TOOL_BYTES` inline cap + spill/truncation only — D-A4's 2000-line `AGENTC_LIMIT_TOOL_LINES` cap is `bash`/`read`-specific and is not part of the extension-tool job path | delivered (P4.3a deviations) |
| D123 | P4.3b delivered deviations: example/test surface | `async_fatal` fails at step index 2; `async_demo.fail_at` replaces the failing step line (no normal `step K` before the error); the `async-demo-debug` command exposes per-tool counters so the golden fixture need not link the example; the extension's own cancellation poll is covered by a direct adapter drive because the driver checks the cancel flag before every step and preempts it | delivered (P4.3b deviations) |
| D124 | P4.3 verification fixes | the start window counts as a live job (link before `start`, retire/free-pending without `stop`, finish the retirement after `start` returns; failed start unlinks with no `stop`); `job_fatal`/`job_timeout` build name-dependent text before `job_cleanup` can release the record; `tests/ext.sh`/`check-ext` bound the harness at 30 s | accepted (P4.3 verification) |
| D125 | P4.4 O-1 terminate is pi-exact | only blocked calls may terminate; the agent ends the submission after the batch only when **every call in the assistant message** was blocked with `terminate`; mixed/normal calls keep the loop. The adapter drops a `terminate` without `block` with a level-1 log | delivered (P4.4a) |
| D126 | P4.4 O-2 no `terminated` field | `agent_end` keeps the assistant `stop_reason` (`tool_use` for a terminated batch); cancel is checked first and wins; an explicit `turn_end.continue` still re-enters once under the shared 32-cap | delivered (P4.4a) |
| D127 | P4.4 O-3 session switch | `session_before_switch` (`override/first/replace/fail-closed`, payload `{"reason":"new"}`) gates RPC `new_session`; blocked/failed/handled-without-decision/malformed/`{"cancel":true}` cancels before any file is touched; an allowed switch emits `session_start{reason:"new", previous_session_file}`; no `session_shutdown` on a switch | delivered (P4.4a) |
| D128 | P4.4 O-4 `user_bash` | stays reserved with no point-table row (unsubscribable/inert); the future site is `rpc_bash` in `src/app/mode_rpc.c` | accepted |
| D129 | P4.4 O-5 `mcp_servers_change` | payload gains per-server `status` (`connecting`/`ready`/`failed`) and post-initialize `caps`; emitted at record creation, READY and FAILED, and lists every non-NULL record | delivered (P4.4b) |
| D130 | P4.4 O-6 MCP reconnect | capped exponential backoff after FAILED (base 1 s `<< min(count,6)`, cap 60 s); `AGENTC_MCP_RETRY_MS` overrides the base (positive clamped `[1,60 s]`, 0 disables, malformed keeps the default); FAILED is non-pending so the startup budget is unaffected; the rearm clears session/sync/caps and re-enters NEW | delivered (P4.4b) |
| D131 | P4.4 O-7 idle stdio drain | READY stdio servers are drained on every pump (at most 64 messages; leftovers buffered), honoring `list_changed` without an exchange; EOF fails the server and arms the reconnect; HTTP has no idle channel | delivered (P4.4b) |
| D132 | P4.4 O-8 deferred set | prompts reload/runtime re-discovery, OSC-11, resumable extension HTTP, image/content, `user_bash`, per-step watchdog, progress ABI (plus structured content and per-extension quotas) stay deferred and demand-gated | accepted |
| D133 | P4.4 reserved points | reserved names are not in the point table: `on` rejects them (level-3, handle 0), `wants` is false, `emit` is inert; a name becomes real only with a table row and an emit site | delivered |
| D134 | P4.4a delivered deviations | `agentc_agent_transport()` + `agentc_mode_rebuild_agent` preserve an injected transport across `set_model`; the mode ctx is the one persistence owner (`mode_flush_observer`, `agentc_mode_note_compaction`) and `agentc_mode_rebind_session` re-publishes context/entry sink + reinstalls the observer after a switch; `agentc_mode_emit_session_start` shares the startup payload | delivered (P4.4a deviations) |
| D135 | P4.4b delivered deviations | `--live-idle` (`mcp_test` mode + mock `MCP_MOCK_IDLE_NOTIFY_MS`); `AGENTC_MCP_RETRY_MS` overrides the base delay only; `agentc_mcp_retry_delay_ms` is internal (`mcp_int.h`); `ext_test` warms the provider/net-mock caches before the allocator baseline; `--live-p2` runs with `AGENTC_MCP_RETRY_MS=0`; `retry_count` resets only at record creation | delivered (P4.4b deviations) |
| D136 | P4.4c process | docs + independent verification + chair commit; the audit's F1 fail-closed fix and F2 caps-name alignment landed before freeze (D137); nothing is pushed | delivered |
| D137 | P4.4 verification fixes | `session_before_switch` treats `sr.blocked` as sticky: a later handler failure/overrun cancels even after an earlier well-formed `{"cancel":false}` (regression test with two handlers); the `mcp_servers_change` `caps` list names the four kinds `tools`/`prompts`/`resources`/`resource_templates` while the `resources` MCP capability still gates both resource list kinds; F3/F4 latent notes recorded in §9.4 | accepted (P4.4 verification) |
| D138 | P4.5 loader surface | `plat` gains `os_ext_open/sym/close/available/suffix`: Linux `-nostdlib` returns `-ENOSYS`/false/NULL with one clear message; macOS `dlopen(RTLD_NOW\|RTLD_LOCAL)`; Windows `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR\|LOAD_LIBRARY_SEARCH_SYSTEM32` and fail-closed if the flags are unsupported | delivered (P4.5a) |
| D139 | P4.5 dynamic location + naming | one fixed directory `<config home>/extensions/`, suffix `.dylib`/`.dll` (ASCII case-insensitive), stem = extension name `[a-z0-9_.:-]{1,64}`, bytewise order, cap 32 with one overflow log; no `extensions.paths`, no project directory, no kill switch (`extensions.disabled` filters by name) | delivered (P4.5a) |
| D140 | P4.5 stem ⇔ descriptor name | the descriptor `out.name` must equal the file stem (fail closed), so config, `--list-extensions` and unload address the library by one name | delivered (P4.5a) |
| D141 | P4.5 close discipline | a dynamic library is closed exactly once and only when idle: registrations removed, `shutdown()` returned, no live async job, no extension callback on the stack; a busy close is deferred to job unlink/pump/shutdown; a mapping still busy at shutdown is kept mapped with a warning (D141-b never-close accepted) | delivered (P4.5a) |
| D142 | P4.5 entry purity + re-entrancy | no ABI change; `agentc_ext_adopt` and the dynamic loader owner-tag the entry phase (entry must be descriptor-only) and roll the owner back on refusal; `load_all` re-checks a record after a re-entrant unload during `init` | delivered (P4.5a) |
| D143 | P4.5 Windows search-order safety | the loader searches the DLL's own directory + System32 only; unsupported search flags fail closed, never a fallback to the default order | delivered (P4.5a) |
| D144 | P4.5 POSIX permissions + trust | POSIX rejects a group/other-writable library file and `extensions/` directory; symlinks are followed so the check is on the target (TOCTOU residual documented); a loaded extension is trusted native code — no sandbox, no crash isolation | delivered (P4.5a) |
| D145 | P4.5 introspection | `AgcExtInfo.dynamic` + a `(dynamic)` marker in `--list-extensions`; the extension ABI itself is unchanged | delivered (P4.5a) |
| D146 | P4.5 composition order | dynamic registration runs in all three composition sites (setup, `--list-extensions`, main) after `register_linked()` and before `apply_config()`, so `AgcExt.order` and `extensions.disabled` apply uniformly | delivered (P4.5a) |
| D147 | P4.5 verification gating | P4.5a is verified locally with the fake loader + cross compiles only; the platform claims (real `dlopen`/`LoadLibraryExW`, ARM64/universal runtime, Gatekeeper/quarantine/signing) are gated on the P4.5b CI jobs (`linux` dynamic gate, `macos-14`, `windows-latest`, Wine `make dyn-check`); no "supported" claim from a local green | accepted (P4.5 verification) |
| D148 | Post-P4 write-first stdio write | `agentc_mcp_write_all` attempts the first `PIPE_BUF` chunk write-first and only an `MC_EAGAIN`/`MC_EINTR` probe falls into the bounded poll path; later chunks poll before writing. Wine's anonymous write pipe matches no `os_poll` branch, so polling before the first byte timed a small request out (`mcp_test write.small=0`, the red `wine-check` ticket). Residual: on Windows writes are blocking and `os_poll` cannot report writability, and on macOS the parent write end is blocking as well, so after the first chunk a full pipe can sleep past the deadline and a request larger than one `PIPE_BUF` still times out; the full fix belongs in `src/plat/win/sys.c` plus a non-blocking macOS parent write end | delivered (post-P4 close-out) |
| D149 | Post-P4 load guard | `agentc_ext_load_all` tracks `g_loading` across its own bounded rounds (a nested `load_all` started from an `init` is not supported: it re-enters the in-progress record, so do not do it); while it is nonzero `agentc_ext_shutdown()` is a logged no-op (`error: ext: shutdown called during load; ignored`), closing the stale-record dereference when an `init()` called shutdown; `agentc_ext_unload` during load stays safe via the post-`init` `!e->used` re-check | delivered (post-P4 close-out) |
| D150 | Post-P4 RPC observer rebind | `agentc_mode_rpc_run` calls `agentc_mode_rebind_session(c)` before returning, so the persistence observer is bound to the caller's `AgcModeCtx` instead of the runner's dead stack frame (`&r.base`); the run's `rpc_event` pump stays frame-bound by design (production tears down after the run) | delivered (post-P4 close-out) |
| D151 | Post-P4 dynamic-home rule | the dynamic scan requires an absolute config home; a relative `XDG_CONFIG_HOME` is refused with `ext: dynamic scan skipped: config home is not absolute` and the scan is skipped, so the optional scan cannot resolve against the process CWD and load libraries from wherever agentc happens to run | delivered (post-P4 close-out) |
| D152 | P5 T-D1 engine modes | `grep`/`find` keep their names and schemas but the backend is a process-wide choice: `--tools-engine external` (default) resolves a per-tool chain and pins the first available rung with the built-in implementation as the last resort; `internal` forces the built-ins and never spawns; there is no `auto` mode (`src/core/tools/engine.h`) | accepted (P5-T1/T2) |
| D153 | P5 T-D2 surfaces | precedence `--tools-engine` > `AGENTC_TOOLS_ENGINE` > `tools.engine` in config.jsonc > `external`; an invalid value warns and falls back to `external`; the getter/setter mirror the `shell` precedent in `config.c` | accepted (P5-T1) |
| D154 | P5 T-D3 chains | `grep`: ripgrep -> POSIX grep -> built-in; `find`: fd -> POSIX find -> built-in; `ls` stays in-process (there is no portable external `ls`; a `fd --max-depth 1` variant is deferred) | accepted (P5-T1/T2) |
| D155 | P5 T-D4 platform degradation | the POSIX `grep`/`find` rungs are POSIX-only: on Windows the chains are `rg -> built-in` and `fd -> built-in`, and Windows `find.exe` (a text searcher) / `findstr` are never invoked | accepted (P5-T1/T2) |
| D156 | P5 T-D5 resolution | resolved once at `builtin-tools` init through the new `os_which_in`/`os_which` PATH probe (absolute result, `PATHEXT` on Windows, execute bit on POSIX, `0`/`-ENOENT`/`-ERANGE`) and pinned for the process; the single info line is emitted by `agentc_tool_engine_log()` from the app startup, not from the extension init, so extension-loading golden tests stay independent of the host PATH | accepted (P5-T1/T2) |
| D157 | P5 T-D6 one vocabulary, one schema | tool names and JSON schemas are identical for every backend and no backend flag is exposed; the per-backend description is generated from the one descriptor table so the model is told the regex dialect and file-selection behavior | accepted (P5-T1/T2) |
| D158 | P5 T-D7 normalization invariants | identical across backends: `path:line:text` (matches and context), the 500-char line cap with `...`, one sorted path per line for find, `no matches\n` on empty, `[grep truncated at N matches]\n` / `[find truncated at N paths]\n`, `limit` as a global cap, `is_error` only for real failures (`rg`/`grep`/`fd`/`find` exit 1 = no matches), and the existing timeout/cancel behavior | accepted (P5-T1/T2) |
| D159 | P5 T-D8 translation | the exact argv per rung lives in `agentc_tool_engine_build_argv` (single source of truth): `rg --json [...] -- PATTERN PATH`; `grep -r -n -H -I (-E|-F) [--exclude-dir .git] -- PATTERN PATH`; `fd --glob [--max-results N+1] -- PATTERN PATH`; `find PATH (-name|-path) PATTERN`. Model arguments are never shell-interpolated and `--` precedes the pattern | accepted (P5-T1/T2) |
| D160 | P5 T-D9 honest deltas | `rg`/`fd` honor `.gitignore`/`.ignore` and skip hidden entries (`fd` also does not follow symlinked directories), `grep`/`find` do not (agentc approximates only `--exclude-dir=.git`); regex dialects differ and the `find` glob translation is approximate; external rungs also sort reader-side while the built-in walks depth-first. These deltas are stated in the generated descriptions | accepted (P5-T1/T2) |
| D161 | P5 T-D10 security/no-network | argv exec only (no shell, no `sh -c` workaround), `--` before the pattern, `os_spawn_group` + `os_kill(-pid)` so no child outlives the run, bounded non-blocking reads, and no downloads (unlike pi, agentc never fetches tools); external children run with an explicit empty environment | accepted (P5-T1/T2) |
| D162 | P5 T-D11 determinism/testing | golden tests pin each rung's argv mapping, parsing, description and the external reader (cap/cancel/timeout) in `tests/engine_test.c`; `--tools-engine internal` is the documented pin for byte-identical prompts; the 8000-entry cap and binary sniffing are built-in-only guarantees | accepted (P5-T1/T2) |
| D163 | P5 T-D12 deferred | external `ls` (`fd --max-depth 1`), per-tool overrides (`tools.engine.grep`), `rg` config-file support, GNU/BSD feature probing, caching/accelerator packs, pi's download fallback (never), pi-style activation defaults, and `glob`->`fd --glob` translation beyond the common cases stay deferred | accepted |

---

## 7. Current authoritative architecture

Full detail in `EXTENSION-SPEC.md`. Summary of what is actually implemented:

### 7.1 The ABI (`include/agentc_ext.h`)

- `AGENTC_EXT_ABI 1`. Structs are append-only; `AgcExtHost`/`AgcExt` carry
  `abi_version`+`struct_size`; contribution structs carry `struct_size` only.
- `AgcExtHost` (31 service slots plus `abi_version`/`struct_size`): `alloc`, `free`, `log`, `out_write`,
  `add_tool`, `add_command`, `add_section`, `add_status`, `on`, `off`, `emit`,
  `defer`, `is_cancelled`, `cwd`, `session_id`, `session_file`, `system_prompt`,
  `append_entry`, `http_request`, `http_cancel`, `notify`, `set_status`,
  `set_model`, `set_thinking`, `request_recompose`, then appended `strdup_`,
  `json_get_str`, `json_get_int`, `json_get_bool`, `json_escape`, `set_title`,
  and (P4.1) `add_provider` (guard with `AGENTC_EXT_HOST_HAS`). The provider
  contribution structs and the registry/lifetime rules are §5.5 and spec §11.1.
- `AgcExt`: `abi_version`, `struct_size`, `name`, `version`, `int32_t order`,
  `init`, `shutdown`, plus the appended `required_host_size` (minimum
  `AgcExtHost.struct_size`; a value above the host size is refused at
  registration). Appended fields/services are read through
  `AGENTC_EXT_FIELD_OK`/`AGENTC_EXT_HOST_HAS`.
- `AgcExtTool.run(host, self, call, out, *is_error)` is synchronous and writes
  through `host->out_write`. Flags are `AGENTC_TOOL_READONLY/DESTRUCTIVE/
  SEQUENTIAL/HIDDEN`, `AGENTC_TOOL_TIMEOUT_DEF_MS 120000`.
- P4.3 appends `start`/`step`/`stop` (0 running / 1 complete / `<0` fatal; five
  stop reasons) plus `AGENTC_EXT_TOOL_TIMEOUT_MAX_MS`; `run` wins when both are
  declared, and start/step output is staged into the job. No host service was
  appended.
- Hook handlers: `int (*)(void *ud, const char *point, const char *payload_json,
  char **result_json)`; `agentc_ext_init` is the only exported symbol, compiled
  per extension with `-Dagentc_ext_init=agentc_ext_<ident>_init`.

### 7.2 Registry, lifecycle, ordering

- `agentc_ext_register_defaults(no_mcp)` registers `builtin-tools`,
  `builtin-context` and (unless `--no-mcp`) `mcp`; `agentc_ext_register_linked()`
  drives `build/exts.c`; `agentc_ext_register_dynamic()` (P4.5, macOS/Windows
  only) scans `<config home>/extensions/` through the platform loader;
  `agentc_ext_load_all()` initializes pending extensions
  lowest `order` first (stable) in bounded rounds (8; anything still pending is
  logged and left pending), sets the owner for the duration of each `init`,
  and rolls back all registrations of a failed init before calling `shutdown`.
  `agentc_ext_describe`/`AgcExtInfo` report `{name, version, order, state,
  disabled, known, dynamic}` in registration order, and
  `agentc_ext_is_loaded(name)` is
  the loaded-state predicate used by the tool policy. A dynamic library is
  closed exactly once when its extension is idle (registrations removed,
  `shutdown()` returned, no live job, no callback on the stack); a busy close is
  deferred to job unlink/pump/shutdown, and a mapping still busy at shutdown
  stays mapped with a warning (D141). `agentc_ext_load_all` counts `g_loading`
  across its bounded rounds (and any nested `load_all` an `init` starts); while
  it is nonzero `agentc_ext_shutdown()` is a logged no-op — the registry is live
  under the load loop — and the process-end shutdown still tears everything
  down (D149). A re-entrant `unload` of the record being initialized is skipped
  by the post-`init` `!e->used` re-check.
- `agentc_ext_tools(NULL,0)` is count-first; the active set is the internal
  `AgcTool` table (run or `start`/`step`), which the app filters with
  `--tools`/`default_tools`/`--no-tools`.
- `agentc_ext_apply_config` excludes default, linked and dynamic extensions named in `extensions.disabled`; it runs after `agentc_ext_register_linked()`/`agentc_ext_register_dynamic()`.
- `agentc_ext_pump` clears the JSON scratch, drains the deferred queue (bounded;
  overflow is counted and logged), runs at most one pending extension HTTP
  request, then runs each registered pump hook with its owner active. It carries
  a re-entrancy guard, so a pump issued from inside a callback returns
  immediately. `agentc_ext_append_entry` validates the custom type/JSON object/
  8 KiB cap before reaching the session sink.

### 7.3 Hook bus

- `on(point, caps, priority, fn, ud)` validates `caps` against the point's
  required capability and returns a handle; `off` removes it. Subscription order
  is `(priority, registration order)`.
- The point table (`registry.c`) declares caps, policy (`first`, `chain`,
  `chain-veto`), merge (`replace`, `fields` with `null` delete) and failure mode
  (open/closed). `tool_call` is `chain-veto`; override points are fail-closed;
  observe points fail-open; three consecutive overruns disable a handler of
  either kind (a clean call resets the counter), with the overrunning occurrence
  blocked on override points and ignored on observe points. Unknown point names
  are rejected at `on`; emits are capped at depth 32 (override blocked, observe
  empty). `agentc_ext_wants(point)` is the emit-site fast path and is backed by
  a live-registration counter. Every callback runs with its owner restored and
  `host->log` prefixed `[ext:<name>] `; failure/overrun diagnostics name the
  extension.
- **P3 note:** the bus is complete and all P3 emit sites are live: trust/
  resources, input/before_agent_start, the direct
  message_end/tool_result/turn_end/settle overrides, provider
  headers/request/response/stream, the compact points, and
  `model_select`/`thinking_level_select` (real-change emissions plus the app
  model/thinking sinks).
- **P4.4a note:** `session_before_switch` is live as the RPC `new_session`
  gate (`override/first/replace/fail-closed`), and `tool_call` carries the
  `terminate` batch hint (blocked calls only; the whole message must agree).
  The other reserved names have no table row (`on` rejects, `emit` inert).

### 7.4 Core as extensions

- `builtin-tools` (`src/core/tools/registry.c`) registers the seven builtins via
  the internal add (so `bash` keeps its async `start`/`step`) and keeps the
  shell-description rewrite.
- `builtin-context` (`src/core/prompt.c`) registers the `context` section
  (project instructions + skills); `agentc_ext_prompt_build` renders the four
  core sections then the extension sections. In auto mode the agent rebuilds
  the prompt from the live tool table before every turn (R1), so a recompose is
  reflected in the next request; `--system` and a `before_agent_start` override
  are stable for the run.
- **P4.2 content layer.** The invocable-prompt registry (`src/core/prompts.c`)
  is core-facing: file templates load once after `resources_discover`, MCP
  prompts register as `mcp__…`, and `/name` / `prompt_template` expand through
  it. The resource root stores (`skillPaths`/`promptPaths`/`themePaths`) live in
  `src/core/resources.c`, and content has no ABI contribution (D97).
- Status providers go through `agentc_status_register`; `status.h` uses the
  `AgcExtStatus*` types.

### 7.5 MCP

- `agentc_mcp_start` reads config and creates records; `agentc_mcp_pump`
  advances the per-server state machine **exactly one server per call,
  round-robin**; `agentc_mcp_tools` is a pure snapshot;
  `agentc_mcp_server_count()` is the configured-server count (a terminal record
  stays counted) and `agentc_mcp_pending()` is the readiness predicate. The
  tool table is bounded
  (256/server, 1024 total), over-long remote names are skipped, descriptions
  truncate at 8 KiB, published names are hash-deduped inside the registry's
  64-byte cap, and the frozen pile of retired-but-handed-out entries is capped
  at 256 and never freed or reused; a full cap fails the server instead of
  recycling (R3; hardened by F1).
- Stdio reads are fully non-blocking; request writes go through the bounded
  `agentc_mcp_write_all` (first `PIPE_BUF` chunk write-first, later chunks
  poll-before-write, deadline-bounded; the Windows write residual is D148).
  HTTP advances one bounded request per pump step
  (wire layer not resumable) and passes its cancel predicate through
  `agentc_http_set_cancel`, so an abandoned request ends with `-ECANCELED`.
  Since P4.4b a READY stdio server is idle-drained on every pump (bounded, 64
  messages; EOF fails it), a FAILED server re-arms behind the capped
  exponential backoff (`AGENTC_MCP_RETRY_MS=0` disables it; FAILED stays
  non-pending), and `mcp_servers_change` publishes each server's
  `connecting`/`ready`/`failed` status plus post-initialize `caps`.
- On a committed `tools/list`, newly discovered tools are published with
  `agentc_ext_add_tool_internal` (which marks the registry dirty) and
  `mcp_servers_change` is emitted; an entry is marked published only on a
  successful add. A retired or replaced entry is withdrawn with
  `agentc_ext_remove_tool_internal(name)` first (P2 fix wave), so the recompose
  sees the new description and stops advertising removed tools.
- **P4.2 capability/prompt/resource sync.** `result.capabilities` picks the
  fetched lists; missing/malformed/empty falls back to tools-only. The connect
  round syncs each advertised kind in order (tools → prompts → resources →
  templates), each with its own pending/again latch, cursor and page cap; a
  `list_changed` is ignored when the capability is not advertised and latched
  during an in-flight sync. A committed `prompts/list` registers
  `mcp__<server>__<prompt>` in the one prompt registry (sync `prompts/get`,
  10 s, text-only, 64 KiB); a committed resource/template list fills per-server
  tables and the first resource-capable sync publishes the three generic
  read-only tools (`mcp_list_resources`, `mcp_list_resource_templates`,
  `mcp_read_resource`) through the internal add, so the next recompose picks
  them up. Tools-cap failures fail the server; prompt/resource cap failures
  abandon that refresh and keep the previous table (P4.2b; D103–D108).
- The app runs a bounded startup pump (400 ms, R1) after trust resolution; it
  exits early once `agentc_mcp_pending()` is false (no record, or every record
  READY/FAILED), so a fast local server's tools are in the startup table; later
  tools appear at the next turn boundary via the recompose hook. A strict
  `--tools mcp__…` name, and the three exact resource-tool names, for a server
  that is still connecting are selected the same way; with MCP disabled the
  deferral is logged as disabled rather than as a pending connection.

### 7.6 Recompose

- The agent calls `a->recompose` after a tool batch fully returns and immediately
  before a terminal `agent_end` on abort/error; never mid-batch.
- `agentc_policy_recompose` checks `agentc_ext_dirty()`, clears it, re-collects
  `agentc_ext_tools`, re-applies the same selection policy, and installs with
  `agentc_agent_set_tools`. It also resolves a deferred strict-`--tools`
  `mcp__` name (D49); a missing non-MCP name was already a startup exit 2.

---

## 8. Verification (as delivered)

All commands run from the repo root. The final pass was from a clean tree
(`make clean`).

| command | result |
|---|---|
| `make` / `make release` | links clean, `build/agentc` runs (`--version` → `agentc 0.1.0-dev`) |
| `make check` | `all tests passed` (all golden suites incl. `ext_test`, `status_test`, `mcp_test`, `agent_loop_test`, `tui_test`) |
| `./tests/ext.sh` | `all extension tests passed` (C example + Rust example + fixture harness) |
| `./tests/mcp.sh` | `all live tests passed` (stdio mock, resync, stubborn-child, new P2 assertions) |
| `./tests/e2e.sh` | exit 0, no failures (mock provider, `--continue`, pty TUI, onboarding) |
| `clang -Wall -Wextra` on the new files | warning-clean |

Not run in this environment: `make windows` / `make wine-check` (cross
toolchain), `make live-net`, `make bench`. P1/P2 did not require them; the
Makefile was updated so the Windows source list still picks up
`src/ext/linked.c`.

**P2 fix + P1 redesign + P3 (final tree).** The whole series was re-verified
from a clean tree after the P3 acceptance audit and the chair's final fixes:
`make` builds `build/agentc` with 0 warnings, `make check` → `all tests passed`
(all golden suites incl. `ext_test`, `status_test`, `mcp_test`, `agent_loop_test`,
`compact_test`, `config_test`, `resources_test`, `policy_test`, `modes_test`,
`tui_test`) plus `gen-exts self-test: 10 cases ok`, `./tests/ext.sh`,
`./tests/mcp.sh` and `./tests/e2e.sh` all green, `--version` →
`agentc 0.1.0-dev`. An independent council audit of P3 reported no blocker; its
findings were fixed in the same tree (invalid JSON for the OpenAI `[DONE]`
stream sentinel, `previous_session_file` only on a real resume, public
`add_tool` flag masking, shutdown on early exits, zero-length request payload,
and the sink provider-switch contract).

**R1–R3b (post-review fix waves) — delivered and verified.** Final clean-tree
run (`c7e45bb` content): `make` → `build/agentc`, 0 warnings; `make check`
(29 golden suites incl. `check-ext`) → `all tests passed` + `gen-exts self-test:
10 cases ok`; `./tests/ext.sh` (C + Rust examples + fixture harness, nix
dev-shell cargo); `./tests/mcp.sh` (58 live checks incl. MCP churn/caps and the
startup pump); `./tests/e2e.sh` (20 checks incl. `--list-extensions` and the
extension summary); `make windows` cross-build green (wine absent). An
independent council verification audited every R1–R3 finding as resolved and
raised one new major (F1: reclaimed MCP slots could be reused under a live
stale tool pointer), fixed before freeze — handed-out entries are never freed
or reused, and a full frozen cap fails the server with a clear log. CI mirrors
the gate: apt clang/make/python3 for `make`, `make ext-pipeline`, `make check`,
then `nix develop -c ./tests/ext.sh` for the Rust half.

**P4.1 (typed custom providers) — delivered and independently verified.**
Clean-tree run: `make` (0 warnings), `make check` (`ext_test`,
`agent_loop_test`, `config_test`, `policy_test`, `modes_test`, `check-ext`),
`./tests/ext.sh` (the harness asserts the linked `fake_provider`),
`./tests/mcp.sh`, `./tests/e2e.sh` (stock; the `--provider fake` probe skips on a
binary that links no manifest entry), `make windows` cross-build, and a
`make release-exts` + `--provider fake --list-models` smoke. The independent
audit found one major (AUTH_NONE discovery sent Bearer when a key resolved) and
five minors; all were fixed with before/after proofs. The per-file map is §5.5.

**P4.2 (skills/prompts/themes + MCP prompts/resources) — delivered and
independently verified.** Clean-tree run: `make` (0 warnings), `make check`
(30 golden suites incl. the new `themes_test`), `./tests/ext.sh`,
`./tests/mcp.sh` (125 live checks), `./tests/e2e.sh`, `make windows`. The
audit found no blocker/major; the three minors (per-list path cap before
validation, MCP prompt argument-description cap, overlapping-exchange sync
retry) and two nits (`skill:` reserved namespace, COLORFGBG overflow) were
fixed with golden and live tests (including a retry sensitivity check).
spec §13 lists the
new/expanded tests (`themes_test` is a new golden suite; `mcp_test` gains the
capability/parser/registry cases; `mcp.sh` gains the live prompts/resources/gate
variants; `tui_test`, `modes_test`, `resources_test` and `policy_test` cover the
TUI/RPC/roots/policy paths).

**P4.3 (async extension tools) — delivered and independently verified.**
Clean-tree run: `make` (0 warnings), `make check` (30 golden suites incl.
`jobs_test`, `ext_test`, `agent_loop_test` + `check-ext`), `./tests/ext.sh`,
`./tests/mcp.sh`, `./tests/e2e.sh`, `make windows`. The audit found a blocker
(record freed while its own `start` was on the stack) and a major (`tool->name`
read after `job_cleanup` could free it); both were fixed with regression tests
that fail when the fix is reverted, plus a 30 s harness bound (D124). The audit
confirmed the ABI, registration matrix, adapter codes, driver cleanup-once,
cancellation/timeout observables, example/tests and security bounds.

**P4.4 (reserved points + completion) — delivered and independently verified.**
Clean-tree run: `make` (0 warnings), `make check` (30 golden suites incl.
`modes_test`/`agent_loop_test`/`ext_test`/`mcp_test` + `check-ext`),
`./tests/ext.sh`, `./tests/mcp.sh` (134 live checks incl. the idle drain and
reconnect), `./tests/e2e.sh`, `make windows`. The audit found one major (a later
`session_before_switch` handler failure/overrun did not cancel when an earlier
handler returned a well-formed `{"cancel":false}`) and one minor (the
`mcp_servers_change` `caps` name list vs the docs); both were fixed with a
regression test and doc alignment (D137). F3/F4 latent notes are recorded in
§9.4. Delivered deviations are D134/D135.

**P4.5 (dynamic extension loading) — delivered, committed; the
platform-runtime half is gated on CI.** Recorded run: `dyn_test` (79 golden
checks with the fake injectable loader) green natively and under Wine;
`tests/dyn.sh --wine` 10/10 green; the Linux `ext-dylib` and Windows DLL
artifacts verified by symbol inspection; macOS arm64/x86_64 and Windows ARM64
cross builds green. **Not verified from this machine:** macOS `dlopen` from the
`-nostdlib` binary, a real Windows `LoadLibraryExW`, ARM64/universal runtime
loading, Gatekeeper/quarantine/signing — those are the CI runtime jobs (the
`linux` dynamic gate, `macos-14` `tests/dyn.sh`, `windows-latest` `--windows`).
P4.5 is committed as feature `9f4b7db` plus docs `9f3c12b`; the full per-file
map is §5.9 and the design/delivery record is `.agents/plans/P4.5-DYNAMIC.md`.

**Post-P4 close-out — delivered in the working tree.** `nix develop -c make
wine-check` → `all 30/30 windows tests passed under wine` (the previously red
`mcp_test write.small=0` is green; the bounded-write check now drains the small
request before the 1 MiB write, because Wine's 4 KiB pipe would otherwise
block the write-first probe on the unread bytes). The close-out tests and the
regenerated goldens: `ext_test` (the ignored shutdown during load, the still-
loaded extension, the re-entrant-unload discrimination, the registry intact
plus later teardown and the allocator baseline), `dyn_test`
(`scan_relative_home`), `modes_test` (`mode.observer.rc`/`mode.observer.
persists` with the frame-poison discrimination). Residual (D148): on Windows an
anonymous-pipe write is blocking and `os_poll()` has no write-pipe branch, so a
request pipe can sleep past the deadline after the first chunk and a request
larger than one `PIPE_BUF` still times out; the full fix belongs in
`src/plat/win/sys.c`. Native macOS/Windows CI observation, `make live-net`,
`make bench`/soak and any tag/release remain open (§9.2/§9.4).

---

## 9. Pending items

### 9.1 P3 — hooks (fully delivered)

Spec §14 P3 landed in three waves (see §5.3 for the file inventory and spec
§4.7 for the emit-site map):

- **Veto consumer:** threaded the rewritten `input` and the `reason` diagnostic
  (delivered).
- **§8 trust reordering:** extensions load before trust resolution;
  `project_trust` is consumed with CLI > hook > saved precedence, `remember`
  persists, and blocked/malformed is fail-closed (delivered; D55–D57).
- **§10 destructive-tool default:** non-core `AGENTC_TOOL_DESTRUCTIVE` tools are
  hidden unless named; D52 resolved to the MCP schema default (delivered; D52,
  D63).
- **`resources_discover`:** project paths gated on resolved trust and root
  validation; only `skillPaths` consumed in P3 (delivered; D63; P4.2 consumes
  all three lists, D108).
- **`before_provider_headers`:** flat patch with the CRLF/key sanitizer and the
  forbidden-header list (delivered; D61).
- **`before_provider_request`:** consuming override; `body_len` follows the
  replacement; a block is terminal via `REQ_BLOCKED` (delivered; D61/D62).
- **`message_end`/`tool_result`:** direct overrides before append/observer with
  their restricted shapes (delivered; D59/D60).
- **`agent_before_settle`/`agent_settled`/`input`/`before_agent_start`:** live
  with `wants` guards (delivered; D53/D58).
- **`model_select`/`thinking_level_select`:** live observe points; the agent
  setters emit on a real change and the app installs the model/thinking sinks
  (delivered; D66). `model_select` also fires on an effective provider/model
  change in `agentc_mode_rebuild_agent`.

The P2 fix wave items (MCP retiral, one-server pump, sync latch, deferred
`mcp__` `--tools`, linked disabling, build scaling) are done and tracked in
§5.2/D46–D51.

The R1–R3b fix waves are also delivered (inventory in §5.4, decisions D67–D80).
Their clean-tree gate run and independent verification are recorded in §8, as
is P4.1's; P4.5 and the post-P4 close-out (§5.10) are committed (`d540dcc`
feature, `6330a82` docs). The only remaining work is demand-gated (§9.2).

### 9.2 P4 — remaining (demand-gated hardening; P5 plans written)

- **P4.2 — delivered, independently verified and committed** (feature
  `c134625`, docs `c8d18a7`; inventory §5.6, decisions D97–D109, spec
  §6.2/§7/§12/§13; the gate run and audit are in §8).
- **P4.3 — async extension tools: delivered, independently verified and
  committed** (inventory §5.7, decisions D110–D124, spec §3.7/§12–§16, design
  record `.agents/plans/P4.3-ASYNC-TOOLS.md`; the gate run and the audit's F1/F2
  fixes are in §8).
- **P4.4 — reserved points and completion: delivered, independently verified
  and committed.** See §5.8, D125–D137 and `.agents/plans/P4.4-COMPLETION.md`.
  Remaining hardening is now demand-gated: prompts reload / runtime
  re-discovery, OSC-11 theme detection, resumable extension HTTP,
  image/content, `user_bash`, per-step watchdog, progress ABI, structured
  content, per-extension quotas.
- **P4.5 — dynamic extension loading: delivered, committed** (feature
  `9f4b7db`, docs `9f3c12b`), platform runtime gated on CI. Linux stays
  static-only; P4.5a (the `plat` loader surface, `dynlib` scan/adopt/close
  policy, registry lifecycle, fake-loader goldens) and P4.5b (`ext-dylib`/
  `win-ext-dylib`, `tests/dyn.sh`, `make dyn-check`, CI jobs) plus the two
  verification fixes are in place. See §5.9, D138–D147 and
  `.agents/plans/P4.5-DYNAMIC.md`; the recorded run and the CI-gated remainder
  are in §8. The post-P4 close-out (§5.10, D148–D151) is committed (`d540dcc`
  feature, `6330a82` docs); see §9.4. Two next-phase plans are written and not
  started: `.agents/plans/P5-AXES.md` (RPC v1, `libagentc` SDK, framed CBOR
  protocol/client/server) and `.agents/plans/P5-TOOL-ENGINE.md` (external
  `rg`/`fd` tool engines with built-in fallback; T-D1–T-D12).
- **Remaining demand-gated work (post-P4 close-out):** native Windows/macOS
  observation of the P4.5 runtime halves (the CI jobs), `make live-net`
  (real-provider TLS/streams), `make bench`/soak runs, an optional tag/release
  when asked, the Windows blocking-pipe residual (`src/plat/win/sys.c`, D148),
  and the frame-bound `rpc_event` ud (safe by design; the persistence observer
  is fixed, D150).
- **P4.1 provider features deliberately left out** (query auth, a custom
  discovery parser, cost rates, per-model thinking, multimodal, OAuth,
  `providers.<id>.headers`, `api` sharing) — §5.5 / D81–D96; design record
  `.agents/plans/P4.1-PROVIDERS.md`. Demand-gated.

### 9.3 Deferred design items

Reserved point names with no core emit site and no point-table row (so `on`
rejects them and `emit` is inert): `session_info_changed`,
`session_before_fork`, `session_before_tree`, `session_tree`,
`cache_warming_decision`, `ui_prompt_start`/`_end`, `user_bash`.
`context`/`context_with_system` fold into `before_provider_request`;
`user_bash`'s future site is `rpc_bash` (`src/app/mode_rpc.c`).

### 9.4 Housekeeping

- **Committed**: the series is on `master` in three logical units (`6c75489`
  ABI/registry, `2329714` hook consumers, `6a524cb` docs), and the plan files
  under `.agents/plans/` are tracked. Nothing is pushed.
- **Post-review fix program committed**: R1–R3b plus the F1 hardening are in
  `c7e45bb` `fix(ext): close the post-review findings (R1-R3)`, with this docs
  commit on top; the plan files are tracked and nothing is pushed.
- **P4.1 committed** by the chair as the typed-provider feature commit
  (`f5de256`) plus its docs commit (`982fe3e`); verified per §8.
- **P4.2 committed** by the chair as the feature commit (`c134625`) plus its
  docs commit (`c8d18a7`); verified per §8.
- **P4.3 committed:** the feature commit carries P4.3a/b + the verification
  fixes — `include/agentc_ext.h`, `src/core/tools/jobs.{c,h}`,
  `src/ext/registry.c`, `crates/agentc-ext/src/lib.rs`,
  `extensions/{manifest.json,async_demo/}`, `tests/{agent_loop_test,ext_test,jobs_test}.c`,
  `tests/data/{agent_loop_test,ext_test,jobs_test}.expected`, `tests/ext.sh`;
  new `extensions/async_demo/` and `.agents/plans/P4.3-ASYNC-TOOLS.md`. P4.3c
  completed; the gate run and independent verification are in §8.
- **P4.4 committed:** the feature commit carries P4.4a/b + the verification fix —
  `include/{agent.h,agentc_ext.h,mcp.h}`, `src/base/limits.h`,
  `src/core/agent.c`, `src/ext/{registry.c,mcp.c,mcp_int.h}`,
  `src/app/{main.c,mode.c,mode.h,mode_rpc.c}`, the P4.4 tests
  (`tests/{modes_test,agent_loop_test,ext_test,mcp_test}.c`, `tests/mcp.sh`,
  `tests/mcp_mock.py`) and the regenerated goldens; verified per §8. Nothing is
  pushed.
- **P4.5 committed:** the feature commit `9f4b7db` carries P4.5a/b plus the two
  verification fixes (`Makefile`, `include/{ext,plat}.h`, `src/app/main.c`,
  `src/ext/registry.c`, `src/plat/mac/mac_internal.h`,
  `src/plat/win/{rt.c,win.h}`, `src/win/kernel32.def`,
  `.github/workflows/ci.yml`, `src/plat/{linux,mac,win}/ext.c`,
  `src/ext/dynlib.c`, `src/ext/dynlib_int.h`, `tests/dyn_test.c`,
  `tests/data/dyn_test.expected`, `tests/dyn.sh`), with docs `9f3c12b`. The
  untracked files were committed together with the tracked modifications, so
  the flake source for any `nix build .#…` release gate is complete. Nothing is
  pushed.
- **Post-P4 close-out committed:** the feature commit `d540dcc` carries
  `src/ext/{mcp,registry,dynlib}.c`, `src/app/mode_rpc.c`, `include/plat.h`,
  the test sources (`tests/{ext,dyn,modes,mcp}_test.c`) and the regenerated
  goldens; the docs commit `6330a82` records §5.10/D148–D151 and the
  PI-EXTENSION-MAP cleanup. Nothing is pushed.
- **Known tickets — both resolved by the post-P4 close-out (D148/D149; §5.10).**
  `nix develop -c make wine-check` failed `mcp_test write.small=0` at HEAD
  `34af8a0` (the CI `linux` job runs `wine-check`, so it was red before
  P4.5); the write-first probe fixes it — the recorded close-out run is 30/30
  green (D148), with the Windows blocking-pipe residual documented in §5.10
  and D148. The pre-existing stale-record dereference when
  `agentc_ext_shutdown()` is called from inside an extension `init()` is
  closed by the `g_loading` guard, which makes the call a logged no-op during
  a load (D149).
- **P3 gate run recorded:** the final P3 tree passed `make check`, `tests/e2e.sh`,
  `tests/mcp.sh`, `tests/ext.sh` and an independent acceptance audit (§8).
- **R-wave gate run recorded:** the R1–R3b clean-tree run and independent audit
  are in §8; the P4.1, P4.2 and P4.3 runs are recorded there too. As of this
  docs pass no gate is open; P4.4's run and audit are recorded in §8.
- **Latent notes (P4.4 audit):** F3 — the observer half is fixed by the
  close-out rebind: `agentc_mode_rpc_run` re-points the persistence observer at
  the caller's ctx before returning (D150; `modes_test mode.observer.persists`
  with the frame-poison discrimination). The run's `rpc_event` pump ud stays
  bound to the stack frame by design — it is the run's own event pump, and
  production emits `session_shutdown`, tears down and returns immediately after
  the run, so nothing observes afterwards; anything that kept observing after
  the run would need a pump rebind too. F4 — `agentc_mode_setup` passes a NULL
  session file for memory-only sessions, which `agentc_ext_set_context` ignores,
  so a stale `session_file` can survive an in-process setup sequence
  (`agentc_mode_rebind_session` handles its own case with `""`). F4 remains
  latent, documented, and untriggered by the app.
- `PI-EXTENSION-MAP.md`'s stale "agentc target" naming column was deleted in
  the close-out; the header now points at D5 and
  `.agents/plans/EXTENSION-SPEC.md` for the authoritative pi-verbatim names.
- `agentc_ext_unload` has no app call path (by design), but the P1-completion
  pass closed the test gap: `ext_test` now checks that an owner's registrations
  are removed before `shutdown()`.
- `set_status` is now a registry segment and is covered by `ext_test`; the
  `set_model`/`set_thinking` sinks are installed by the app
  (`agentc_mode_setup`, cleared in teardown, P3) and `model_select`/
  `thinking_level_select` emit on a real change.
- HTTP MCP connect step is bounded but retried from scratch on timeout; a
  wire-layer stepping API would remove the P2 deviation (D25).
- Re-sync failure on an already-ready server abandons the refresh and keeps the
  previous tool table (intentional; matches the old `mcp_server_sync_tools`).

---

## 10. Review findings summary (spec v2 fresh-eyes pass)

Full disposition in `EXTENSION-SPEC.md`'s appendix. The material ones:

**Blockers fixed in v2:** `out_write` was promised but absent; `AgcExtToolCall`
opaque with no arg/cancel access; async tools had no output/error channel; the
custom-provider vtable was unimplementable; no unregister/owner (dangling
callbacks); `request_recompose` from `pump` caused a use-after-free mid-job;
`tool_result`/`message_end` overrides ran after persistence; observe/override
overrun was fail-open.

**Majors fixed:** `--no-mcp`/`--no-tools`/strict `--tools` threading and the
`agentc_ext_tools` signature; `status.h`/`status_test.c`/`plugin_int.h` missing
from migration; five hardcoded Makefile symbols; `gen-plugins.py` table shape;
`agentc_plugin_adopt` story; Rust crate rewrite scope; flag-value/`THREADSAFE`
change; the `wants` fast path; `tool_execution_update` removal; `project_trust`
ordering; resources trust gating; header injection/`Content-Length`;
destructive-tool default; `append_entry` validation; emit return for decision
points; chain-vs-first for `tool_call`; merge algebra; capability gating; handler
error protocol; `agentc_mcp_tools` blocking re-sync; print-mode acceptance.

**Nits fixed:** line references, `SETTLE` wording, `compact_now` location,
"every struct carries `abi_version`" corrected.

---

## 11. Environment and working agreement

- **Repo:** `/home/pvl/spaces/abird/src/agentc`. Run all commands from there.
- **pi reference:** `/home/pvl/spaces/abird/research/pi` (monorepo: `durable`,
  `mcp`, `coding-agent`, …). The `pi-web` bundle ships
  `pi-coding-agent@1.0.0` with `dist/core/extensions/types.d.ts`.
- **Build/test:** `make`, `make check` (golden suite), `./tests/e2e.sh`,
  `./tests/mcp.sh`, `./tests/ext.sh` (was `plugin.sh`). All four must stay green.
  `make windows`/`win-tests`/`wine-check` exist for cross builds.
- **Hard rules** (`.agents/AGENTS.md`): freestanding C23; only
  `<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>`; no OS headers outside
  `plat/`/`net/`; errors are negative errno; memory via
  `agentc_alloc/realloc/free`; one module per file with a contract header; golden
  tests for parsers/formatters; never add a private status path.
- **Makefile note:** `src/*.c` is auto-discovered; `tests/*.c` auto-builds but
  needs `tests/data/<name>.expected`. `EXT_REG := build/exts.c` is regenerated
  from `extensions/manifest.json` by `tools/gen-exts.py`; `build/exts.mk` is a
  real make target (atomic write, errors fail the build) and `make check`
  includes `check-ext`, which runs the linked examples.
- **Style:** design docs are agent-facing and kept in sync with code in the same
  commit. The four extension docs are `.agents/docs/30-extensibility.md`,
  `.agents/docs/20-core-agent.md` §4/§6, `extensions/README.md`, and the spec.

---

## 12. Quick reference — where everything lives

| concern | authoritative location |
|---|---|
| the spec to implement | `.agents/plans/EXTENSION-SPEC.md` (v3) |
| P4.1 provider design + deviations | `.agents/plans/P4.1-PROVIDERS.md` |
| P4.3 async-tool design + deviations | `.agents/plans/P4.3-ASYNC-TOOLS.md` |
| P4.4 reserved-point/completion design + deviations | `.agents/plans/P4.4-COMPLETION.md` |
| P4.5 dynamic-extension design + deviations | `.agents/plans/P4.5-DYNAMIC.md` |
| post-P4 close-out record (D148–D151) | this file §5.10 |
| journey, decisions, inventory, pending | this file |
| current extension design record | `.agents/docs/30-extensibility.md` |
| current core design (tools/prompt) | `.agents/docs/20-core-agent.md` §4/§6 |
| review ledger | `.agents/docs/REVIEW.md`; spec appendix |
| pi 1.0.0 API | `.agents/plans/PI-EXTENSION-MAP.md` |
| pi 2.0 durable/mcp | `.agents/plans/PI-2-DURABLE-MCP-MAP.md` |
| earlier designs | `.agents/plans/EXTENSION-TOOL-ENGINE.md`, `EXTENSION-MINIMAL.md` |
| build rules | `.agents/AGENTS.md`, `Makefile` |
| the ABI | `include/agentc_ext.h` |
| core registry API | `include/ext.h` |
| internal bus | `src/ext/registry_int.h` |
| registry + hook bus | `src/ext/registry.c` |
| dynamic loader | `include/plat.h` (`os_ext_*`), `src/plat/<os>/ext.c`, `src/ext/dynlib.{c,int.h}` |
| generated table | `src/ext/linked.c`, `build/exts.c`, `build/exts.mk`, `tools/gen-exts.py`, `extensions/manifest.json` |

---

## 13. Starting prompt for the next agent (post-P4 close-out)

Pass the text below verbatim to the next agent.

---

> Work in `/home/pvl/spaces/abird/src/agentc`.
>
> You are picking up the `agentc` extension architecture **after the P4
> close-out**. **P1–P3, the post-review fix waves R1–R3b, P4.1, P4.2, P4.3,
> P4.4 and P4.5 are implemented, verified and committed.** The **post-P4
> close-out** (write-first stdio write D148, load guard D149, RPC observer
> rebind D150, dynamic relative-home rule D151; inventory §5.10) is committed
> (`d540dcc` feature, `6330a82` docs) — `nix develop -c make wine-check` is
> 30/30 green and the two old tickets are resolved (§9.4). Nothing is pushed;
> commit only when asked.
>
> The remaining work is **demand-gated hardening plus the two written P5 plans**:
> `.agents/plans/P5-AXES.md` (RPC v1 / `libagentc` SDK / framed CBOR protocol)
> and `.agents/plans/P5-TOOL-ENGINE.md` (external `rg`/`fd` tool engines with
> built-in fallback). Neither has started.
>
> - **Optional:** keep the full gate set green: `make check`,
>   `./tests/e2e.sh`, `./tests/mcp.sh`, `./tests/ext.sh`, `make windows`,
>   `make ext-dylib`, `make win-ext-dylib`, and (with Wine)
>   `nix develop -c make wine-check` (30/30) / `make dyn-check`.
> - **Observe the platform-runtime halves in CI:** `macos-14` `tests/dyn.sh`
>   (real `dlopen` from the `-nostdlib` binary), `windows-latest`
>   `tests/dyn.sh --windows` (real `LoadLibraryExW`, ARM64/universal runtime,
>   Gatekeeper/signing) and the windows-latest golden-suite shell runs
>   (informational until observed green). A local Linux green proves only the
>   fake-loader lifecycle and the cross builds (D147).
> - **Environment-gated gates not run here:** `make live-net` (real
>   provider/TLS), `make bench` and any soak run; an optional release tag when
>   asked.
> - **Known residual (D148):** Windows anonymous-pipe writes are blocking and
>   `os_poll()` has no write-pipe branch, so a full request pipe can sleep past
>   the write deadline after the first `PIPE_BUF` chunk and a request larger
>   than one `PIPE_BUF` still times out; the complete fix belongs in
>   `src/plat/win/sys.c`. Small requests and the 30/30 Wine suite are
>   unaffected.
> - **Known latent note:** the run's `rpc_event` event-pump ud (`RpcCtx *r`)
>   is frame-bound to `agentc_mode_rpc_run` by design — the persistence
>   observer is rebound to the caller's ctx (D150) and production tears down
>   immediately after the run, so nothing dangles today. Anything that kept
>   observing after the run would need a pump rebind too.
> - **Demand-gated feature hardening** (spec §14, handoff §9.2/§9.3):
>   prompts reload / runtime re-discovery, OSC-11 `system` theme detection,
>   resumable extension HTTP, image/content channel, `user_bash` emit site,
>   per-step watchdog, progress ABI, structured content, per-extension
>   quotas; and the P4.1 provider features deliberately deferred (query auth,
>   a custom discovery parser, cost rates, per-model thinking, multimodal,
>   OAuth, `providers.<id>.headers`, `api` sharing).
>
> Before doing anything, read, in order:
>
> 1. `.agents/plans/EXTENSION-HANDOFF.md` — the master worklog (goal, journey,
>    every decision D1–D151, the P1/P2/P3, R1–R3b, P4.1–P4.5 inventories, the
>    post-P4 close-out, pending items, environment).
> 2. `.agents/plans/EXTENSION-SPEC.md` (v3) — the authoritative spec,
>    especially §2/§3.5 (registration + entry purity), §3.7 (async extension
>    tools), §4 (hook points, policy, merge algebra, failure modes; the
>    terminate batch rule in §4.2), §4.7 (live emit sites + the job-driver
>    note), §5 (registry, composition order, dynamic close discipline, the
>    shutdown-during-load no-op), §6.2 (prompt templates/`/skill:`/themes),
>    §7 (MCP capabilities, prompts, resources, idle drain/reconnect, the
>    bounded stdio write), §8 (trust ordering + the P4.4a/close-out session
>    rebind), §9/§10 (config + security, incl. the dynamic loader and the
>    absolute-home rule), §11.1 (P4.1 custom providers), §12 (limitations),
>    §13 (testing), §14 (phasing).
> 3. `.agents/plans/P4.1-PROVIDERS.md`, `.agents/plans/P4.3-ASYNC-TOOLS.md`,
>    `.agents/plans/P4.4-COMPLETION.md` and `.agents/plans/P4.5-DYNAMIC.md` —
>    the design/delivery records with their delivered deviations and deferrals.
> 4. `.agents/docs/30-extensibility.md` and `.agents/docs/20-core-agent.md`
>    §3/§4/§6 — the current design records; `.agents/docs/10-platform.md` §1.3
>    for the Windows pipe/poll behavior.
> 5. The code: `include/agentc_ext.h` (the ABI), `include/plat.h` (`os_ext_*`),
>    `include/ext.h`, `src/ext/registry_int.h`, `src/ext/registry.c` (the async
>    block, the lifecycle + `g_loading` guard, the provider block, the P4.4
>    tool_call/terminate adapter, the HTTP cancel path, and the P4.5
>    `DynRec`/close/trampoline blocks), `src/ext/dynlib.{c,int.h}`
>    (`AgcExtDynLoader`, scan, path policy, the absolute-home rule),
>    `src/plat/{linux,mac,win}/ext.c` (the three loaders),
>    `src/plat/win/sys.c` (`os_poll`/`os_write` — the write residual),
>    `src/core/agent.c` (the all-terminate batch), `src/app/mode_rpc.c`
>    (the session switch + observer rebind), `src/ext/mcp.c` (idle drain,
>    retry/backoff, `mcp_servers_change`, the write-first `agentc_mcp_write_all`).
>
> Constraints: follow `.agents/AGENTS.md` hard rules (freestanding C23, no libc,
> negative errno, `agentc_alloc`). Keep `make check`, `./tests/e2e.sh`,
> `./tests/mcp.sh` and `./tests/ext.sh` green. If a spec detail is wrong against
> the code, fix the spec in the same change and say so. Do not commit unless
> asked.

---

## 14. Appendix — the exact P1/P2/P3 file inventory

Tracked-status snapshot (the series is committed; inventory below):

**Added (now committed on `master`):** `include/agentc_ext.h`, `include/ext.h`,
`src/core/prompt.h`, `src/ext/registry.c`, `src/ext/registry_int.h`,
`src/ext/linked.c`, `tools/gen-exts.py`, and the six `.agents/plans/` files.

**Deleted:** `include/agentc_plugin.h`, `include/plugin.h`, `src/ext/plugin.c`,
`src/ext/plugin_int.h`, `tools/gen-plugins.py`.

**Renamed:** `plugins/` → `extensions/`; `crates/agentc-plugin/` →
`crates/agentc-ext/`; `tests/plugin_test.c` → `tests/ext_test.c`;
`tests/plugin.sh` → `tests/ext.sh`; `tests/data/plugin_test.expected` →
`tests/data/ext_test.expected`.

**Modified:** `Makefile`, `README.md`, `include/agent.h`, `include/config.h`,
`include/mcp.h`, `include/status.h`, `src/app/main.c`, `src/app/mode.c`,
`src/app/mode.h`, `src/core/agent.c`, `src/core/config.c`, `src/core/events.c`,
`src/core/events.h`, `src/core/prompt.c`, `src/core/status.c`,
`src/core/status_builtin.c`, `src/core/tools/registry.c`, `src/ext/mcp.c`,
`src/tui/tui.c`, `tests/agent_loop_test.c`, `tests/data/status_test.expected`,
`tests/e2e.sh`, `tests/mcp.sh`, `tests/mcp_test.c`, `tests/status_test.c`,
`tests/tui_e2e.py`, `tests/tui_test.c`, `.agents/AGENTS.md`, `.agents/README.md`,
`.agents/docs/*.md`, `.agents/plans/EXTENSION-SPEC.md`,
`.agents/plans/EXTENSION-HANDOFF.md`, `extensions/README.md`,
`extensions/hello/hello.c`, `extensions/manifest.json`,
`extensions/rust_echo/{Cargo.toml,Cargo.lock,src/lib.rs}`,
`crates/agentc-ext/{Cargo.toml,src/lib.rs}`.

The P1-completion pass also expands the renamed `tests/ext_test.c` (merge
semantics, observe-1 rejection, snapshot dispatch, dirty, unload order,
`set_status`) and adds the `--system --dump-wire` check to `tests/e2e.sh`.

**P2 fix wave (same change set):** `src/ext/mcp.c` (retiral and
replacement via `agentc_ext_remove_tool_internal`, publish marks registered only
on success, one-server round-robin pump, in-flight `list_changed` latch),
`src/app/main.c` (strict `--tools` defers `mcp__` names), `src/ext/registry.c`
(`agentc_ext_apply_config` covers defaults and linked names; retire seam),
`Makefile` and `tools/gen-exts.py` (manifest-driven build list; invalid
sanitized-identifier rejection). `tests/mcp_test.c` already asserts the
retiral/replacement path (a changed description yields a fresh entry, a removed
tool disappears from the next snapshot); the latch and `--tools`-deferral paths
belong to the wave's verification.

**P3 (same change set):** added `src/app/project.{c,h}`,
`src/app/policy.{c,h}`, `tests/policy_test.c`, `tests/data/policy_test.expected`;
modified `src/core/agent.c`, `src/app/main.c`, `src/app/mode.c`,
`src/ext/{mcp,registry}.c`, `include/agent.h`, `src/core/tools/registry.c`,
`tests/agent_loop_test.c`, `tests/config_test.c`, `tests/resources_test.c`,
`tests/compact_test.c`, `tests/data/*.expected`,
`.agents/docs/30-extensibility.md` and `.agents/docs/20-core-agent.md`.
Wave 4 added `model_select`/`thinking_level_select` emit sites
(`src/core/agent.c`, `src/app/mode.c`) and the app model/thinking sinks
(`agentc_mode_setup`), with tests in `tests/agent_loop_test.c` and
`tests/modes_test.c`.
