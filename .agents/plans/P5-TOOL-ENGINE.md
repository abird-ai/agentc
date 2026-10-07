# P5 — tool engine selection (external `rg`/`fd`/`grep`/`find` vs built-in)

Status: **implemented (as-built).** Complements `.agents/plans/P5-AXES.md`
(RPC/SDK/protocol, axes A/B/C) — this plan is the core-tools axis and is
independent of it; either can land first. The as-built behavior is documented in
`.agents/design/20-core-agent.md` §4.5; the as-built decisions are recorded here
as T-D1…T-D12.

## 0. Repo hygiene — read before doing anything

- **Do not start implementation on a dirty tree.** At plan time the working
  tree carries a parallel session's unrelated WIP (~96 modified files: TUI,
  tools, providers incl. a new `src/prov/google.c`, tests, goldens) plus an
  untracked `.agents/plans/P5-AXES.md`. Decide with the chair whether to
  commit, stash, or branch that WIP first; this plan was written against
  `6330a82` and touched none of it.
- This plan changes **core tools only** (`src/core/tools`, `plat.os_which`,
  config/CLI/docs). It does **not** touch the extension ABI
  (`include/agentc_ext.h`), the registry, the RPC/protocol work in
  `P5-AXES.md`, or the parallel WIP files.
- Keep `make check`, `./tests/ext.sh`, `./tests/mcp.sh`, `./tests/e2e.sh`
  green; `nix develop -c make wine-check` (30/30) and `make dyn-check` should
  stay green too.

## 1. Goal

`grep`, `find` and `ls` stay the same tools with the same JSON schemas and
names, but the **backend** becomes a user choice:

- default: prefer the environment's native searchers (`ripgrep`, then POSIX
  `grep`; `fd`, then POSIX `find`) and fall back to the built-in freestanding
  implementations;
- opt-in: force the built-in implementations (hermetic, offline,
  deterministic).

The built-ins remain the always-available last resort; they are never
*preferred* when the user has not opted in. This mirrors pi (which delegates
`grep`→ripgrep and `find`→fd) while keeping agentc's zero-dependency promise
and never downloading anything.

## 2. Current state (facts to build on)

**agentc built-ins** (`src/core/tools/registry.c:247–260`): `read`, `ls`,
`find`, `grep`, `write`, `edit`, `bash`, all in-process freestanding C.
- `grep`/`find` share the `.gitignore`-aware walker in `find.c` (bounded to
  8000 visited entries); binary files are sniffed and skipped; `ls` is
  in-process and sorted.
- Output conventions to normalize against (`grep.c:208–219`, `find.c:275–293`):
  - grep: `path:lineno:text` for matches **and** context lines; 500-char line
    cap with a `...` suffix; `no matches\n` when empty;
    `[grep truncated at N matches]\n`, `[grep stopped after 8000 entries]\n`.
  - find: one path per line (the built-in emits depth-first tree order);
    `no matches\n` when empty;
    `[find truncated at N paths]\n`, `[find stopped after 8000 entries]\n`.
- Schemas (unchanged by this plan):
  - `grep`: `{pattern, path, glob, ignore_case, literal, context, limit}`
    (only `pattern` required).
  - `find`: `{pattern, path, limit}` (only `pattern` required).
  - `ls`: `{path, limit}`.
- Built-ins are registered by the `builtin-tools` default extension via
  `agentc_ext_add_tool_internal` (`registry.c`), so the backend can be chosen
  at extension `init` time, after config is loaded.
- Activation: agentc activates **all** builtins unless `default_tools` is
  set (`main.c` "all" policy branch). pi activates only
  `["read","bash","edit","write"]` by default; changing agentc's activation
  default is **out of scope here** (T-D12).

**Platform**: `os_spawn` is raw `execve` (no `PATH` search; `src/plat/linux/sys.c`
`linux_spawn_common`), `os_spawn_group` + `os_kill(-pid, sig)` give process-group
kill, `os_pipe` gives a non-blocking read end, `os_wait` polls. There is **no**
`os_which` today; MCP works around bare commands with
`/bin/sh -c 'exec "$0" "$@"'` (`src/ext/mcp.c:2310–2317`) — we do **not** reuse
that shell trick.

**pi reference** (`research/pi/packages/coding-agent`):
- `createCodingTools` = `read, bash, edit, write` (the default active set:
  `DEFAULT_TOOL_NAMES = ["read","bash","edit","write"]` in
  `core/settings-manager.ts:215`).
- `createReadOnlyTools` = `read, grep, find, ls`.
- `createAllTools` = `read, bash, powershell, edit, write, grep, find, ls`
  (the registry; `defaultTools`/`--tools` select the active subset).
- `grep` spawns ripgrep (`core/tools/grep.ts:119,168`), `find` spawns fd
  (`core/tools/find.ts:172,216`); both resolve `PATH` first and otherwise
  **download** the binary (`utils/tools-manager.ts`, disabled by `PI_OFFLINE`).
  `ls` is in-process. Tools are factories with an injectable `operations` seam.

## 3. Decisions (T-D1…T-D12)

- **T-D1 — Modes.** `--tools-engine external|internal`.
  - `internal`: force the built-in implementations; never spawn.
  - `external` (**default**): resolve a per-tool chain and use the first
    available rung; the built-in implementation is the last resort. Log the
    chosen rung once.
  There is no separate `auto` mode (with an internal last resort, it would be
  identical to `external`).
- **T-D2 — Surfaces.** CLI `--tools-engine <mode>`; config
  `"tools": { "engine": "external" }`; env `AGENTC_TOOLS_ENGINE`. Precedence:
  flag > env > config > default (`external`). Invalid values log and use the
  default.
- **T-D3 — Chains.**
  - `grep`: `rg` → POSIX `grep` → built-in.
  - `find`: `fd` → POSIX `find` → built-in.
  - `ls`: built-in only. There is no portable external `ls` (locale/format,
    Windows `dir`), and pi keeps `ls` in-process as well. A `fd --max-depth 1`
    variant is deferred (T-D12).
- **T-D4 — Platform degradation.** POSIX `grep`/`find` rungs are POSIX-only.
  On Windows the chains are `rg → built-in` and `fd → built-in`; never invoke
  Windows `find.exe` (a text searcher) or `findstr`.
- **T-D5 — Resolution.** Resolve the chain once at `builtin-tools` init via a
  new `os_which(name, out, cap)` (PATH scan; `PATHEXT` on Windows; executable
  bit on POSIX; absolute result; `0 | -ENOENT`). The chosen backend is pinned
  for the process; recompose does not re-resolve. One startup line:
  `info: tools: grep -> rg (Rust regex); find -> fd; ls -> built-in`.
- **T-D6 — One vocabulary, one schema.** Tool names and JSON schemas are
  identical for every backend; the model never sees or passes backend flags.
  The **description** is generated per effective backend from one descriptor
  table (no hand-written branches) so the model knows the regex dialect, glob
  semantics and file-selection behavior. No `engine` argument is exposed.
- **T-D7 — Normalization invariants** (identical across backends): output
  format (`path:line:text` for grep; one path per line for find — the external
  reader bytewise-sorts, the built-in emits depth-first tree order;
  500-char grep line cap with `...`), `no matches\n` on empty, truncation
  markers `[grep truncated at N matches]\n` / `[find truncated at N paths]\n`,
  `limit` enforced as a global cap, `is_error` only for real failures
  (exit 1 from `rg`/`grep` = no matches, not an error), binary files skipped,
  default `path` `.`, and the existing timeout/cancel behavior.
- **T-D8 — Translation.** Exact argv/parsing/limits per rung in §6. Model
  arguments are never shell-interpolated; `--` terminates option parsing.
- **T-D9 — Honest deltas.** `rg`/`fd` honor `.gitignore`/`.ignore` and skip
  hidden entries; `grep`/`find` do not (we approximate only
  `--exclude-dir=.git` for `grep` and nothing for `find`). Regex dialects
  differ. These deltas are stated in the generated description, with the
  escape hatch (“to include ignored/hidden files, pass an explicit `path`” or
  use a glob). Exact parity across backends is impossible; we do not pretend
  otherwise.
- **T-D10 — Security/no-network.** argv exec only (no shell); `os_spawn_group`
  + `os_kill` on cancel/timeout so children cannot outlive the job; bounded
  non-blocking reads; no downloads (unlike pi — agentc never fetches tools);
  `--` before the pattern so `-`-prefixed patterns cannot become flags.
- **T-D11 — Determinism/testing.** Golden tests pin each rung's argv mapping,
  parsing and rendered description. For reproducible evals or environments
  where a byte-identical prompt matters, `--tools-engine internal` is the
  documented pin. The built-in tests keep running under `internal`.
  8000-visited-entry and binary-sniff guarantees are built-in-only; external
  rungs are bounded by `limit`, the output caps and the tool timeout.
- **T-D12 — Deferred.** External `ls` (`fd --max-depth 1` variant); per-tool
  engine overrides (`tools.engine.grep`); `rg` config-file support; GNU/BSD
  feature probing beyond `-Z` (e.g. `--exclude-dir`); caching/accelerator packs;
  pi's download fallback (**never**); pi-style activation defaults
  (`read/bash/edit/write` only); `glob`→`fd --glob` translation beyond the
  common cases.

## 4. Architecture and files

```
include/agentc.h or include/agent.h   (only if a public getter is wanted)
include/plat.h                        os_which() declaration
src/plat/{linux,mac,win}/*.c          os_which() implementations
include/config.h, src/core/config.c   tools.engine parsing + getter
src/app/main.c                        --tools-engine flag, help text
src/core/tools/engine.h               (new) internal seam: descriptors, resolve,
                                      description builder, external run fns
src/core/tools/engine.c               (new) descriptor table, os_which-backed
                                      probe, spawn/read/cap helpers, the four
                                      external run functions
src/core/tools/registry.c             builtin-tools init selects .run/.desc
                                      per tool from the resolved engine
tests/engine_test.c + tests/data/engine_test.expected   (new golden suite)
tests/e2e.sh                          external-mode smoke (stub rg on PATH)
.agents/design/20-core-agent.md       §4 tools, §6 config, §8 flags
.agents/design/30-extensibility.md    one line: builtin tools are engine-selected,
                                      extensions unaffected
```

Design notes:
- The built-in run functions stay exactly where they are; this plan only adds
  new `run`/`desc` selection in the `builtin-tools` extension init.
- The descriptor table is the single source of truth for both the argv
  mapping and the description text.
- `engine.h` is core-internal (like `prov/provider.h`), so `tests/engine_test.c`
  can drive both selection and the external run functions directly.

## 5. Backend descriptor and generated descriptions

```c
typedef struct {
    const char *engine;        /* "ripgrep" | "grep" | "built-in" */
    const char *regex_clause;  /* one clause, e.g. "Rust regex (no backreferences)" */
    const char *glob_clause;   /* "gitignore-style globs" | "fnmatch --include globs" */
    const char *ignore_clause; /* "skips .gitignore/hidden" | "searches ignored/hidden (only .git skipped)" */
    bool        skips_binary;  /* always true; kept for the text */
    bool        limit_global;  /* always true after normalization; kept for the text */
} AgcToolEngineDesc;
```

Rendered descriptions (proposed; pinned by goldens):

- **grep / ripgrep** — “Search file contents with ripgrep (rg). `pattern` is a
  Rust regular expression (backreferences and lookaround are not supported);
  set `literal: true` for plain text. Respects `.gitignore`/`.ignore` and
  skips hidden files unless `path` names one. Output is `path:line:text` with
  an optional context window; lines are capped at 500 characters.”
- **grep / POSIX** — “Search file contents with the system grep (POSIX
  extended regex: no `\d` classes, no lazy quantifiers); set `literal: true`
  for plain text. Ignored and hidden files are searched (`.git` is skipped
  best-effort); binary files are skipped. Output is `path:line:text` with an
  optional context window; lines are capped at 500 characters.”
- **grep / built-in** — the current text, plus “(built-in engine)”.
- **find / fd** — “Find files whose path or name matches a glob pattern
  (`*`, `?`, `**`, `[abc]`). Uses fd: respects `.gitignore`, skips hidden
  entries and does not follow symlinked directories. Output is one path per
  line.”
- **find / POSIX** — “Find files with the system find. Pattern translation is
  approximate (`**` may not match zero directories); `.gitignore` and hidden
  files are not skipped. Output is one path per line.”
- **find / built-in** — the current text, plus “(built-in engine)”.
- **ls** — unchanged (built-in).

## 6. Translation (exact)

### 6.1 grep

| rung | argv |
|---|---|
| `rg` | `rg --json [--ignore-case] [-F] [--glob GLOB] [--context N] [--max-count LIMIT] -- PATTERN PATH` |
| `grep` | `grep -r -n -H -I (-E or -F) [-i] [--include GLOB] [-C N] [--exclude-dir .git] [-m LIMIT] -- PATTERN PATH` |
| built-in | `agentc_tool_grep_run` (unchanged) |

- `literal:true` → `-F` (drop `-E`); `ignore_case` → `-i`; `context` →
  `-C`/`--context`; `glob` → `--glob`/`--include`; `limit` → `--max-count`
  **per file** plus a global cap enforced by the reader (stop at `limit`
  matches, kill the child, append `[grep truncated at N matches]`).
- `rg --json`: consume `match` and `context` events, normalize each to
  `path:line:text` (strip the trailing newline, apply the 500-char cap with
  `...`). `rg` exit 1 = no matches → `no matches\n`; exit 2 → stderr text as
  an error result.
- POSIX `grep`: lines use `:` separators for matches and `-` for context
  (`file-line-text`); normalize the first two separators to `:` by finding the
  `[-:]<digits>[-:]` boundary. Filenames containing `:` or `-<digits>-` are
  parsed by the first boundary (documented limitation; `rg` is recommended).
  Exit 1 = no matches; exit 2 = error.
- Built-in-only guarantees (not promised on external rungs): the 8000-entry
  visit cap and byte-level binary sniffing.

### 6.2 find

| rung | argv |
|---|---|
| `fd` | `fd --glob --max-results LIMIT+1 -- PATTERN PATH` |
| `find` | `find -H PATH [-name PATTERN | -path PATTERN]` |
| built-in | `agentc_tool_find_run` (unchanged) |

- `fd`: empty output → `no matches\n`; read lines, sort bytewise for
  deterministic output, cap at `limit` with `[find truncated at N paths]\n`.
- POSIX `find` pattern mapping: no `/` in the pattern → `-name PATTERN`;
  otherwise → `-path PATTERN` verbatim (find's own `**`/`/` behaviour is
  documented as approximate; recommend `fd`/built-in). Includes hidden and gitignored
  files; sort the reader output for determinism and apply the same cap/marker.
- `find`/`fd` return 0 for an empty result; only real spawn/exec errors set
  `is_error`.

### 6.3 Caps, cancellation, errors

- Read child output non-blockingly with the existing pipe pattern; enforce
  the same output caps as the built-ins (grep line cap 500; result caps where
  the built-ins have them) and stop reading + kill the group when the cap or
  `limit` is reached.
- Timeout from the tool's existing job timeout; `is_cancelled`/abort kills the
  process group.
- Spawn failures (`-ENOENT` after `os_which` raced) → clear error text naming
  the backend and suggesting `--tools-engine internal`.

## 7. Config / CLI / logging

- `--tools-engine internal|external` (help text: “external = prefer ripgrep/fd/
  grep/find from PATH, fall back to the built-in implementations”).
- `"tools": { "engine": "external" }` (user and project config; project scope
  follows the existing trust rules for project config).
- `AGENTC_TOOLS_ENGINE` env override.
- Startup log (info): `tools: grep -> rg (Rust regex); find -> fd; ls -> built-in`
  or `tools: grep -> built-in; …` under `internal`.
- `--offline` does not change the engine choice (external tools are local; we
  never download).

## 8. Security

- argv exec only; no shell, no `sh -c` workaround (MCP's workaround stays
  where it is).
- `--` before the pattern; patterns starting with `-` cannot become flags.
- `os_spawn_group` + `os_kill` on cancel/timeout; no orphaned children.
- Bounded reads; no unbounded buffering.
- No network access; never download tools.

## 9. Tests

New `tests/engine_test.c` + `tests/data/engine_test.expected` (golden, runs in
`make check`):
- `os_which`: present/absent/exec-bit/PATHEXT cases using a temp `PATH` and
  executable stub scripts created by the test.
- Selection matrix: `rg` present → ripgrep; only `grep` → POSIX; neither →
  built-in; `--tools-engine internal` never spawns (stubs record calls).
- Argv mapping per rung: an env-controlled stub that prints its argv (one
  line) and canned output; assert the exact command line for grep
  (`rg`/`grep`) and find (`fd`/`find`), including `--`, `-F`, `-i`, `-C`,
  glob, limit.
- Parsing: `rg --json` match/context normalization; POSIX `-`/`:` separators;
  `no matches` on exit 1; error text on exit 2; 500-char cap; `limit`
  truncation markers; find sort + cap.
- Descriptions: render each backend's `grep_desc`/`find_desc` and pin them.
- Cancellation/caps: a stub that emits unbounded output must be killed at the
  cap; a stub that sleeps must be killed on abort/timeout.
- Keep `tools_test` (internal) green; run the internal tests under
  `--tools-engine internal` semantics (engine default does not affect them
  because the test harness calls the built-in run functions directly).

`tests/e2e.sh`: an external-mode smoke with a stub `rg` on `PATH` (tool
round-trip) plus the existing internal run; assert the prompt names the
backend.

## 10. Phasing (each wave: implement → full gates → independent verify → commit)

- **T1 — grep.** `os_which` + descriptor/resolve + `grep` rg and POSIX rungs +
  generated grep descriptions + `engine_test` grep cases + docs for the flag/
  config/grep.
- **T2 — find.** `fd` and POSIX `find` rungs + find descriptions + tests.
- **T3 — close-out.** e2e external smoke, docs
  (`.agents/design/20-core-agent.md` §4/§6/§8, one line in
  `.agents/design/30-extensibility.md`), goldens, independent verification,
  commit.

Accepted for all waves: `make check`, `./tests/ext.sh`, `./tests/mcp.sh`,
`./tests/e2e.sh`, `make windows`; `nix develop -c make dyn-check` and
`nix develop -c make wine-check` (30/30) stay green.

## 11. Risks and open items

1. **POSIX grep parsing** — filenames containing `:`/`-<digits>-` are
   ambiguous. Resolved where the local grep supports it: R5 probes `-Z` by
   observing a NUL on stdout and parses NUL-delimited records (ADR-10/ADR-11);
   a grep that lacks `-Z` keeps the best-effort parser, so `rg` is still
   recommended there.
2. **`find` glob translation** — `**` cannot be expressed exactly; document
   and recommend `fd`/built-in; never silently return wrong results.
3. **GNU vs BSD feature variance** — start with the portable subset; probe
   only if a real need appears.
4. **Description changes prompt bytes** — engine-dependent descriptions change
   the system prompt and goldens; that is intended and pinned per rung.
5. **Deterministic evals** — pin `--tools-engine internal`; document.
6. **Parallel WIP** — do not build on or commit the other session's tree;
   resolve §0 first.
7. **`fd` exit codes / output order** — resolved: `fd` exit 1 = no match
   (`exit1_nomatch`), and the reader sorts bytewise; no `--sort` dependency.
8. **Windows** — only `rg`/`fd` rungs exist; POSIX rungs must never be
   attempted.

## 12. Handoff — starting prompt for the implementing agent

Pass this verbatim:

---

> Work in `/home/pvl/spaces/abird/src/agentc`.
>
> Read, in order: `.agents/AGENTS.md`; `.agents/plans/P5-TOOL-ENGINE.md` (this
> plan — the authoritative scope/decisions T-D1…T-D12);
> `.agents/design/20-core-agent.md` §4/§6/§8; `src/core/tools/registry.c`,
> `grep.c`, `find.c`, `ls.c`;
> `include/plat.h`; `src/plat/linux/sys.c` (spawn); `include/config.h` and
> `src/core/config.c`. Reference only: `.agents/plans/P5-AXES.md` (the A/B/C
> axis, independent) and pi at `/home/pvl/spaces/abird/research/pi`
> (`packages/coding-agent/src/core/tools/{index,grep,find,ls}.ts`,
> `src/utils/tools-manager.ts`, `src/core/settings-manager.ts:215`).
>
> **First check the tree is clean.** If `.agents/plans/P5-AXES.md` or ~96
> unrelated modified files (a parallel session's WIP) are present, stop and ask
> the chair to freeze/commit/stash them; do not build on that WIP and do not
> commit it.
>
> Implement T1 (grep: `rg` → POSIX `grep` → built-in) with: `os_which` in
> `plat`; the engine descriptor table and description builder in new
> `src/core/tools/engine.{c,h}`; backend selection in the `builtin-tools` init;
> `--tools-engine`/`tools.engine`/`AGENTC_TOOLS_ENGINE`; the normalization and
> security rules of §6/§8; and `tests/engine_test.c` + golden plus the docs
> updates. Then T2 (find: `fd` → POSIX `find` → built-in) and T3 (e2e, docs,
> verification). Keep everything green; commit only when asked.
>
> Acceptance: the default is `external` with the chain and built-in as the
> last resort; `internal` forces the built-ins and spawns nothing; tool names,
> schemas and normalized output are unchanged; descriptions name the effective
> backend and its dialect; no shell, no downloads; `make check`,
> `./tests/ext.sh`, `./tests/mcp.sh`, `./tests/e2e.sh`, `make windows`,
> `nix develop -c make dyn-check` and `nix develop -c make wine-check` stay
> green. Update `.agents/design/20-core-agent.md` §4/§6/§8 and one line in
> `.agents/design/30-extensibility.md` in the same change, and record
> T-D1…T-D12 as decisions when landing.
