# HANDOFF — agentc model-discovery, TUI pickers/commands, and inline-resize work

Written 2026-10-08, end of the session that produced commits `3a3f5e6 … 8c83a1b`.
Audience: the next coding agent (or maintainer) picking this work up cold.
Read this file top to bottom; it is the single entry point. Deeper records are
linked under `.agents/runs/`, `.agents/plans/` and `.agents/design/`.

---

## 0. TL;DR / state

- Repo: `/home/pvl/spaces/abird/src/agentc` — `agentc`, a freestanding C23 coding
  agent (no libc, static binaries, mbedTLS, MCP, extension C ABI). `VERSION` 0.5.0.
- Branch `master`, **HEAD `8c83a1b`**, **11 commits ahead of `origin/master`**, 0 behind.
- Working tree **clean**.
- All suites green: `make check` (32 golden suites + extension pipeline) and
  `./tests/e2e.sh` (mock provider, pty TUI, onboarding), run with a clean `HOME`.
- **One open work item: inline-mode resize** (the region ghosts when a terminal
  repositions the cursor across a refow). Root cause and the chosen fix
  (**Design 2**) are specified in `.agents/plans/INLINE-RESIZE-DESIGN.md`. Not yet
  implemented; the tree is intentionally at the last known-good state.

---

## 1. Repo orientation (read these first)

- `.agents/AGENTS.md` — working agreement, **hard rules**, layout, build/test gates.
  The hard rules matter:
  - Freestanding: only `<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>`. No
    stdio/stdlib/string/unistd. Public contracts are `agentc.h`, `plat.h`, `net.h`,
    `wire.h`, `agent.h`, `session.h`, `config.h`, `oauth.h`, `mcp.h`, `ext.h`,
    `agentc_ext.h` — **extend by appending, never edit existing fields/signatures**.
  - Core calls Layer 0 (`plat.h`) / Layer 1 (`net.h`) only; OS code lives in
    `src/plat/<os>` and `src/net/<os>`.
  - Memory: `agentc_alloc/realloc/free`; no stdout/stderr except
    `agentc_out*`/`agentc_log*`.
  - One module per file; header comment states the contract; golden tests for every
    parser/formatter.
- `.agents/README.md` — index of agent-facing material. `design/NN-*.md` describes the
  **current** implementation and must be updated in the same commit as a behaviour
  change; `plans/` is forward-looking; `runs/<slug>/` holds per-run `PLAN.md`/`STATUS.md`.
- Build/test (run from the repo root):
  - `make` → `build/agentc`; `make test` → `build/test/<name>`; `make check` (gates).
  - `./tests/e2e.sh` (mock provider + pty TUI + onboarding; no network).
  - `./tests/mcp.sh`, `./tests/net.sh`, `./tests/ext.sh`, `make windows`.
  - Before committing: `make check` must be green; provider/TUI changes also need
    `./tests/e2e.sh`.
- **Test-environment (resolved):** `tests/run.sh` and `tests/ext.sh` now isolate
  `HOME`/`XDG_*` for the golden binaries, so `ext_test`'s `mem_baseline` is
  hermetic and no longer reads the developer's real
  `~/.config/agentc/{auth,models-cache,setup}.jsonc`. `make check` and
  `./tests/e2e.sh` are green with the real `$HOME`; set `AGENTC_TEST_HOME` to pin
  the scratch dir. The old `env HOME=/tmp/agentc-ci-home` workaround is no longer
  required (it remains harmless).

---

## 2. Baseline and history

- The session before ours ("fix-review-council", 8 commits, last `04bc319`) landed a
  review-and-fix set across net/wire/TLS, tools, core agent, TUI and extensions. Its
  record is `.agents/runs/fix-review-council/`.
- **Our session starts at `04bc319`** and produced the 11 commits below. The
  model-discovery engine already existed in the base (commit `360f8a0`, in
  `origin/master`); our job was to complete/correct it and build the TUI features on
  top.

### Work log (newest first)

| commit | subject | notes |
|---|---|---|
| `8c83a1b` | feat(discover,config,tui): configurable Codex client_version, Anthropic limit, menu wrap, medium default | config+env client version; Anthropic `limit=1000`; slash-menu wrap; `default_thinking` really medium |
| `1e7ecaa` | fix(discover): Codex /models requires a client_version capability | the Codex backend gates models by `client_version` semver |
| `87c58a0` | feat(setup,discover): interactive setup picker and Codex model discovery | setup uses the picker; Codex `/models`; `agentc_discover_models_ops` |
| `4068855` | feat(tui): inline scrollbar when the live tail exceeds the region | `comp_scrollbar` |
| `272a905` | fix(tui): robust inline resize erase and terminal restore on every path | `\x1b[J` erase; signal handlers installed before raw mode |
| `ebb7cf0` | feat(tui,app): in-TUI /resume\|/continue, one picker, medium default | shared `PickList`; retired standalone picker; `AgcTuiApp` services; resume/new mode APIs |
| `ae2a73d` | docs(readme): link the opcode assembly research sibling | intro pointer + Related section |
| `bfed267` | feat(tui): /thinking picker, /compact and a real /new session | shared `agentc_mode_new_session`; `AgcTuiApp` |
| `29ce8c4` | feat(tui,app): session resume picker for --continue/--resume | `agentc_session_summary`, standalone picker |
| `ae6a52e` | feat(tui): interactive /model picker with full-width selection | modal picker; `comp_command_menu` prefix + full-width band |
| `3a3f5e6` | feat(discover): complete auto model discovery for supporting providers | Anthropic auth, refresh force, gateway metadata, Ollama edges, ext sanitize |

Per-run records: `.agents/runs/auto-discovery-council/` (PLAN+STATUS),
`.agents/runs/tui-commands-council/PLAN.md`,
`.agents/runs/tui-session-switch-council/PLAN.md`.

---

## 3. Feature specs (what is implemented, where, why)

### 3.1 Auto model discovery (`src/core/discover.c`, `include/discover.h`)

- `agentc_discover_models(provider, base, key, out, max, timeout, err, errap)` — public
  entry; resolves the provider row **by name**.
- `agentc_discover_models_ops(ops, base, key, …)` — row-driven entry used by the
  setup/auto-pick path so the two `openai` rows (chat vs Codex) resolve to the chosen
  row. Declared in `src/prov/provider.h` (internal).
- Styles (`src/prov/provider.h`): `AGENTC_DISCOVER_DEFAULT` (`GET {base}/models`),
  `_ANTHROPIC` (`GET {base}/v1/models?limit=1000` + `anthropic-version`),
  `_OLLAMA` (native `GET {host}/api/tags`, fallback `{host}/v1/models`),
  `_GOOGLE` (`GET {base}/models`), `_CODEX`, `_NONE`.
- Auth: each row may set an internal `auth_headers` hook; when set it is authoritative
  (no Bearer fallback). Builtins that use it:
  - **Anthropic** (`src/prov/anthropic.c`): `x-api-key` for API keys,
    `authorization: Bearer` + `user-agent`/`x-app`/`anthropic-beta` for `sk-ant-oat`
    OAuth tokens — mirroring the request adapter.
  - **Google** (`src/prov/google.c`): `x-goog-api-key`.
  - **Codex** (`src/prov/codex.c`): `authorization: Bearer` + `chatgpt-account-id`
    (via `agentc_oauth_account_id`) + `originator: agentc` +
    `openai-beta: responses=experimental`.
  - Builtin Bearer fallback sanitizes control bytes.
- Parsers:
  - `parse_openai` reads `data[]` (or `models[]`): `id`/`name`, `display_name`, plus
    gateway metadata `context_window`/`context_length`/`top_provider.context_length`,
    `max_output_tokens`/`top_provider.max_completion_tokens`,
    `architecture.input_modalities` (or the **input half** of `architecture.modality`;
    an explicit `vision` wins), and a `reasoning`/`include_reasoning` entry in
    `supported_parameters`.
  - `parse_ollama_tags`: `models[].name`/`model`, `details.family`/`parameter_size`.
  - `parse_google`: `models[].name` (strips `models/`), `displayName`,
    `inputTokenLimit`/`outputTokenLimit`, reasoning heuristic.
  - `parse_codex`: `models[].slug`/`display_name`, `context_window`,
    `input_modalities`, `supported_reasoning_levels`; skips rows with
    `supported_in_api:false`; keeps hidden rows (so `gpt-5.5` etc. show).
- Cache: `~/.config/agentc/models-cache.jsonc`, per provider, `{fetched, base, models}`;
  TTL 24h; a changed non-empty `base` is a miss; merged provider-wise.
- Registry: discovered rows register via `agentc_model_register_dynamic` (2048-slot
  runtime table in `src/prov/models.c`); `agentc_model_find` checks the **static
  catalog first**, then dynamic, so known models keep their catalog metadata.
- Wiring (`src/app/setup.c`): `agentc_setup_discover(cfg, name, live, offline, force)`
  is cache-first; network only when `live && !offline`, a key exists (unless
  `needs_key==0`), and the cache is empty/stale or `force`. `--refresh-models` forces
  it even when a configured model resolves. `agentc_setup_list_models` walks usable
  providers; `choose_model` asks the picker.
- **Codex `client_version`** (the subtle one): the ChatGPT Codex backend requires a
  semver `client_version` query param and returns only models whose
  `minimal_client_version` is ≤ it. agentc keeps its own identity (`originator:
  agentc`) and declares a **capability level**: default `1.0.0`, resolved from
  `AGENTC_CODEX_CLIENT_VERSION` → `OPENAI_CLIENT_VERSION` → config
  `openai_client_version` → default. Published by
  `agentc_discover_set_codex_client_version()`, called from
  `agentc_setup_set_context()`. Live-verified: 10 models returned; `0.5.0` returns 0.
- `include/discover.h` header comment lists the endpoints.

### 3.2 TUI pickers (`src/tui/picklist.*`, `src/tui/tui.c`, `src/tui/components.*`)

- `src/tui/picklist.{c,h}` — **one** selection mechanic: substring filter, scroll,
  **wrap-around Up/Down**, PageUp/PageDown paging, Enter/Esc/Ctrl-C. `PickList` holds
  base rows (borrowed) + filtered view (`vname`/`vdesc`/`vidx`). `PICKLIST_MAX` 128.
- In-TUI pickers (`Tui`): `menu_kind` = `TUI_MENU_COMMAND` (the slash menu) |
  `_MODEL` | `_THINKING` | `_SESSION`; `pick_open`, `PickList pick`, owned
  `pick_name`/`pick_desc`/`pick_paths`. `tui_pick_mirror()` republishes the visible rows
  through the `menu_*` fields so the existing layout/render path is unchanged.
  `tui_model_pick_open`/`tui_thinking_pick_open`/`tui_session_pick_open` build the base
  rows; `tui_pick_key` maps the `PickList` action to model/level/session.
- `src/tui/components.c` `comp_command_menu(g,th,x,y,w,maxrows,names,descs,n,top,sel,
  prefix)`: optional `prefix` (`/` for commands, NULL for pickers) and a **full-width
  reverse band** for the selected row (`grid_fill` then text).
- `comp_scrollbar(...)`: bottom-anchored region shows a one-column scrollbar when the
  uncommitted tail exceeds the region; the thumb tracks `chat.scroll`.
- `--continue`/`--resume` on a tty open the **in-TUI** session picker at startup
  (`AgcTuiApp::pick_session_on_start`); the mode opens the newest first, Esc keeps it.
- `/resume`/`/continue` work **at any point**: while a run is in flight, set
  `switch_pending` and `agentc_agent_abort`; the picker opens once the submit has
  unwound (so tools/requests are cancelled first).

### 3.3 TUI commands (`src/tui/tui.c`)

Registered in `tui_commands[]` (order preserved for goldens):
`clear, help, model, new, quit, theme, thinking, compact, resume, continue`.
- `/model [id]` — no arg opens the model picker (runtime catalog for the current
  provider, restricted to the current model's wire); `/model <id>` sets directly.
- `/thinking [level]` — no arg opens the level picker (marks the live level read from
  the agent); `/thinking <level>` sets directly.
- `/compact` — `agentc_agent_compact()` then reports `compacted` / `nothing to
  compact` / error; `compact_done` is reset at the start of every compaction so a
  no-op cannot report a stale success.
- `/new` — starts a **real** new session via `AgcTuiApp::new_session`
  (`agentc_mode_new_session`), then clears the view; refuses while running.
- `/resume`, `/continue` — session picker (see 3.2).
- `/clear` — clears the view (allowed mid-run); `/theme`, `/help`, `/quit`.

### 3.4 App services (`src/tui/tui.h`)

```c
typedef struct {
    void *ud;
    int (*new_session)(void *ud);
    int (*resume_session)(void *ud, const char *path);
    const char *session_dir;
    bool pick_session_on_start;
    void (*on_compact)(void *ud, const AgcCompactInfo *ci);
} AgcTuiApp;
int agentc_tui_run(AgcAgent*, const char *initial_prompt, int mode,
                   const char *theme_name, bool trusted, bool show_tools,
                   const AgcTuiApp *app);
```
`main.c` passes `{ .ud=&mc, .new_session=tui_app_new_session,
.resume_session=tui_app_resume_session, .session_dir=session_dir,
.pick_session_on_start=tty_session_pick, .on_compact=tui_mode_compact }`.

### 3.5 Session swap APIs (`src/app/mode.{c,h}`)

- `int agentc_mode_new_session(AgcModeCtx*)` — cancel-veto (`-125`), create new file,
  rebind persistence, clear transcript, `session_start("new")`. Shared with RPC
  `new_session` (the RPC stdout session header stays in `mode_rpc.c`, **not** in the
  shared function — calling it from the TUI would corrupt the screen).
- `int agentc_mode_resume_session(AgcModeCtx*, const char *path)` — open, replay into
  the agent, rebind, resync `flushed`, `session_start("resume")`.
- `include/session.h`: `agentc_session_summary(path,&ts,preview,cap)` (header stamp +
  first user line, bounded read) and `agentc_session_age_label(ts,now,out,cap)`.

### 3.6 Setup / onboarding picker (`src/app/setup.c`, `src/tui/pick.{c,h}`)

- `src/tui/pick.{c,h}` — a **standalone** picker for pre-TUI flows, built on the same
  `PickList` + `comp_command_menu`; `agentc_tui_pick(Terminal*, …)` is driveable with
  the memory backend, `agentc_tui_pick_tty(…)` opens the tty, installs restore
  handlers, enters raw/alt mode and runs.
- `setup_pick(title,names,descs,n,initial)` returns `-2` when stdin is not a terminal,
  so scripts/pipes and the golden tests keep the **numbered line menu** fallback.
- Used for: provider menu, preset/extension submenu, credentials method (subscription
  vs API key), and the model list (`choose_model`).

### 3.7 Providers

- **OpenAI chat** (`openai-chat`): `DEFAULT`, Bearer. **OpenAI Codex**
  (`openai-codex-responses`): `CODEX`, OAuth. Two rows share the name `openai`;
  builtins register chat first, and `agentc_discover_models_ops` / setup use the
  chosen row to disambiguate.
- **Anthropic**: `ANTHROPIC`, auth hook (`x-api-key`/Bearer), `limit=1000`.
- **Google (native)**: `GOOGLE`, `x-goog-api-key`.
- **Ollama (local)**: `OLLAMA`, no key, `/api/tags` → `/v1/models`.
- **Ollama Cloud**: `DEFAULT`, Bearer (`OLLAMA_CLOUD_API_KEY`/`OLLAMA_API_KEY`).
- Presets (`openrouter`, `xai`, `deepseek`, `groq`, `mistral`, `together`, `gemini`):
  `DEFAULT`.

### 3.8 Defaults and terminal hygiene

- **Reasoning default is medium.** Two places: `src/app/main.c` (`thinking_set ?
  thinking_flag : 3`) **and** `src/core/config.c` (`default_thinking = dup_if("medium")`).
  The config default was the one that actually mattered — before it was `"off"` and
  overrode the app fallback.
- **Terminal restore on every path** (`src/tui/tui.c`): SIG*/atexit restore is installed
  **before** `term_enter_mode`; the enter-failure path clears the registered cleanup;
  `tui_emergency_restore` + `term_leave` reset the pushed OSC-11 background. The
  inline erase uses `\r\x1b[J` from the parked region top (see the open item below).

---

## 4. Design decisions (why, not just what)

1. **Discovery is provider-row driven, not name driven** — two `openai` wires share a
   name; `agentc_discover_models_ops` exists so the caller's chosen row wins.
2. **One picker mechanic (`PickList`)** — model/thinking/session pickers and the
   standalone setup picker share it; wrap-around and filtering live in one place.
3. **Owned picker row storage** — model ids/session paths are copied into the `Tui`,
   so no catalog/session pointer can dangle while the picker is open.
4. **Modal pickers swallow every key**; the slash-command menu is a separate,
   editor-driven completion UI (it also wraps now).
5. **`AgcTuiApp` is the app-services table** — extend it, don't widen
   `agentc_tui_run`.
6. **The session-header stdout write is RPC-only** — never from the shared
   `agentc_mode_new_session` (it would print JSON into the TUI).
7. **Mid-run session switches defer** — abort first, open the picker after the submit
   unwinds; never swap while a tool/request is live.
8. **`agentc setup` keeps a non-tty fallback** — scripting and the golden suite must
   not depend on a tty picker.
9. **Codex client identity vs capability** — `originator: agentc` is identity;
   `client_version` is a semver capability declaration. Non-semver 400s; a low value
   silently hides all models.
10. **Reasoning default moved to `config.c` too** — the app fallback alone was a no-op.

---

## 5. Open work / pending items

### 5.1 Inline-mode resize (the main open item)

Full problem statement, investigation log, and the chosen **Design 2** are in
`.agents/plans/INLINE-RESIZE-DESIGN.md`. Summary: the inline region relies on a hidden
cursor parked at its top; some terminals reposition/reflow the cursor on resize, so
the erase misses and one composer ghost accumulates per resize (grow-then-shrink
reproduces it on the reporter's terminal; tmux is clean). Chosen fix: **Design 2 —
screen-owned bounded window, flush-to-scrollback, row-1 cursor park, `\x1b[2J`
repaint on resize** (bounded memory, exact seam, deterministic resize). Not
implemented.

### 5.2 Smaller pending / optional items

- `session_before_compact` cancel currently reports as "nothing to compact"; a distinct
  "cancelled" message would be nicer (low).
- `/compact` runs synchronously with no busy indicator (low).
- `prompt_command.shadow` test asserts dedup via an `/h` filter (kept meaningful); fine.
- `src/prov/openai.c` writes `r->api_key` unsanitized for a *request* (pre-existing;
  discovery/Anthropic/Google/ext paths sanitize). Consider parity (low, security).
- The Codex `client_version` default `1.0.0` is a capability guess; if OpenAI raises the
  bar, bump the constant or set `OPENAI_CLIENT_VERSION` (documented).

---

## 6. Verification protocol

```sh
cd /home/pvl/spaces/abird/src/agentc
make check              # 31 golden suites + ext pipeline (HOME/XDG isolated)
./tests/e2e.sh          # mock provider + pty TUI + onboarding
```
- `make check` must be warning-free.
- A provider/TUI change also runs `./tests/e2e.sh`.
- Golden files live in `tests/data/*.expected`; the runner is `tests/run.sh` (byte-for-
  byte stdout+stderr). When adding `check(...)` labels, regenerate the golden.
- `tests/e2e.sh` runs `tests/setup_e2e.py` (pty drives the picker; file/pipe tests drive
  the numbered fallback) and `tests/tui_e2e.py` (pty TUI, menu placement, resize storm,
  SIGTERM restore).
- `tests/pty_screen.py` is the terminal emulator used by `tui_e2e.py`; it now understands
  `ESC[J`.
- `tests/run.sh`/`tests/ext.sh` isolate `HOME`/`XDG_*` (set `AGENTC_TEST_HOME` to choose
  the scratch dir); the golden binaries never see the real `$HOME`.

---

## 7. File map (what lives where)

- Discovery: `include/discover.h`, `src/core/discover.c`, `src/prov/provider.h`,
  `src/prov/provider.c`, `src/prov/{openai,anthropic,google,codex}.c`,
  `src/app/setup.c`.
- Model catalog/registry: `src/prov/models.c`, `include/agent.h`.
- Session: `src/core/session.c`, `include/session.h`, `src/app/mode.{c,h}`,
  `src/app/mode_rpc.c`.
- Config: `include/config.h`, `src/core/config.c` (`openai_client_version`,
  `default_thinking`).
- TUI: `src/tui/tui.{c,h}`, `src/tui/picklist.{c,h}`, `src/tui/pick.{c,h}`,
  `src/tui/components.{c,h}`, `src/tui/tui_test.h`.
- App wiring: `src/app/main.c`, `src/app/mode.c`.
- Tests: `tests/discover_test.c`, `tests/config_test.c`, `tests/session_test.c`,
  `tests/tui_test.c`, `tests/agent_loop_test.c`, `tests/data/*.expected`,
  `tests/e2e.sh`, `tests/setup_e2e.py`, `tests/tui_e2e.py`, `tests/pty_screen.py`.
- Design docs (kept in sync): `.agents/design/20-core-agent.md` (§3.8 discovery, §6.3
  resume), `.agents/design/30-extensibility.md` (§2.7 ext auth sanitize),
  `.agents/design/40-tui.md` (§1.4 resize, §6 pickers/commands). `README.md`
  (interactive use, opcode sibling).

---

## 8. Known pitfalls / cautions

- **Two `openai` rows.** Any new discovery/registry code must go through the chosen
  row (`agentc_discover_models_ops`), not `agentc_provider_by_name("openai")`.
- **Never write the RPC session-header to `io->write` from the TUI path.**
- **`tui_submit` in test mode leaves `running==true`** (harness quirk); commands that
  guard on `running` must be tested with that in mind.
- **Modal pickers must swallow keys**; the command menu must keep its editor-driven
  semantics.
- **`agentc_model_find` prefers the static catalog** — dynamic discovery cannot shadow
  a catalog entry; add catalog rows deliberately.
- **The inline resize item (5.1) is a real defect** on some terminals; do not claim it
  fixed by the current `\x1b[J` erase.

---

## 9. Starting prompt for the next agent

Paste the following to the next agent:

> You are continuing work on `agentc` at `/home/pvl/spaces/abird/src/agentc`
> (freestanding C23 coding agent; branch `master`, HEAD `8c83a1b`, tree clean, all
> suites green with a clean HOME). Read `.agents/plans/HANDOFF.md` in full first — it
> is the complete context: repo rules, the exact commit log of the work already
> landed (model discovery, TUI pickers and commands, session resume/new, setup
> picker, Codex/Anthropic/Ollama discovery, thinking default medium), the design
> decisions, verification protocol, and pending items. Then read
> `.agents/plans/INLINE-RESIZE-DESIGN.md`, which specifies the one open work item:
> inline-mode resize ghosts on terminals that reposition the cursor across a reflow.
>
> Your task: implement **Design 2** from that document end-to-end — a screen-owned
> bounded window for inline mode, flush-to-scrollback for older rows, the cursor
> parked where a height-shrink cannot scroll it, and `\x1b[2J` + repaint on resize —
> so resize is deterministic and memory stays O(window) instead of O(transcript).
> Migrate the inline/storm goldens from the old "commit to scrollback" model to
> "window on screen, committed rows in the terminal scrollback exactly once", update
> `.agents/design/40-tui.md` §1.4 accordingly, and verify with both tmux (a real
> reflowing terminal, script in the handoff's spirit) and
> `env HOME=/tmp/agentc-ci-home make check && env HOME=/tmp/agentc-ci-home
> ./tests/e2e.sh`. Do not touch the public headers except by appending; do not
> regress the other features listed in the handoff. Record the run under
> `.agents/runs/<slug>/` and commit when green.
