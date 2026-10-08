# review-fixes-council

Chair: main agent. Scope: resolve every confirmed finding from the fresh review
of `agentc@8c83a1b` cleanly (code + tests + docs), keeping `make check` green.

## Ground rules
- Single tree, no sibling worktrees (repo instruction: operate in tree only).
- Disjoint file ownership per workstream; a worker never edits a file owned by
  another stream. Shared golden files are owned by exactly one stream.
- Build isolation: `make ... TST_OBJDIR=build/obj/wsN/test TST_BINDIR=build/wsN/test`
  and run the binary directly with an isolated `HOME`/`XDG_*`. Never run the
  shared `make check` while another stream is building.
- Every behavior change lands with its golden test updated in the same stream.

## Workstreams
### Wave 1 (parallel, disjoint files/tests)
- WS1 harness: tests/run.sh, tests/ext.sh (hermetic HOME/XDG).
- WS2 base: src/base/json.c, glob.c, fmt.c, deadline.h; json/fmt/base/fuzz tests.
- WS3 ext+wire: src/ext/mcp.c, registry.c; src/wire/http.c; mcp/http_replay tests.
- WS4 tui: src/tui/markdown.c, tui.c; tui_test.
- WS5 tools/agent: tools/{bash,engine,read,edit,find,ls}.c; core/{messages,agent}.c;
  tools/engine/agent_loop tests.

### Wave 2 (after wave 1 gates)
- WS6 core state: core/{config,session,resources,discover}.c, prov/models.c,
  include/{config,discover}.h; config/session/resources/discover tests.
- WS7 providers/app: prov/{openai,codex,provider}.c, app/{setup,main,mode,mode_rpc}.c;
  provider/modes tests.
- WS8 docs: README.md, .agents/design/*.

### Wave 3
- Independent verification (council + reviewers), then full `make check`,
  e2e.sh, mcp.sh, ext.sh on an isolated HOME.

## Decisions (chair)
- D1 test hermeticity: run.sh/ext.sh export HOME + XDG_* to a scratch dir.
- D2 model registry: clear all dynamic rows at the start of a discovery pass
  (static extension rows preserved); keep the 256 cap per pass as a hard bound.
- D3 auth precedence: code follows the documented order
  flag > OAuth > env > auth.jsonc > config.
- D4 HTTP: TE overrides CL; non-final chunked => close-delimited.
- D5 EINTR: every read/wait/poll retries on -EINTR.
- D6 docs are corrected to the shipped behavior where a feature is intentionally
  absent (/fork, image wire support), and implemented where cheap and promised
  (Retry-After HTTP-date, ${ARGUMENTS}, custom providers.<id> gateway).
