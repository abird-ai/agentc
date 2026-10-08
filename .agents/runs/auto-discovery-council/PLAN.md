# auto-discovery-council — plan

Chair: main agent. Goal: complete "auto model discovery for providers that support
it" in agentc. Recon (three scouts) established that the discovery engine already
exists end-to-end (`src/core/discover.c`, `src/app/setup.c`, `src/prov/*`,
`AGENTC_DISCOVER_*` styles, cache, dynamic registry). This run closes the concrete
gaps that make auto-discovery wrong or unavailable for providers that DO support
listing.

No worktrees. Streams own disjoint files and MUST NOT run `make`/`make check`
(one working tree, one build dir); the chair builds and runs the suite after all
streams land. Chair owns all test/golden edits to avoid golden conflicts.

## Findings driving the work
- **Anthropic discovery auth mismatch (High).** The request adapter uses
  `x-api-key` for API keys and `authorization: Bearer` for `sk-ant-oat` OAuth
  tokens (`src/prov/anthropic.c` `write_headers`), but discovery always sends
  `authorization: Bearer` (`src/core/discover.c` `AGENTC_DISCOVER_ANTHROPIC`).
  `include/discover.h` documents "x-api-key or Bearer". Anthropic supports
  listing; API-key users silently get no models.
- **`--refresh-models` is a no-op with a resolved default model (High).** In
  `src/app/main.c` discovery runs only inside `if (!model ||
  !agentc_model_find(provider, model))`, so the documented "ignores the cache"
  refresh never fires when the configured model resolves.
- **OpenAI-compatible metadata is guessed (Medium).** `parse_openai` reads
  non-standard `context_window`/`max_output_tokens`/`vision`; OpenRouter's real
  fields (`context_length`, `top_provider.context_length`,
  `top_provider.max_completion_tokens`, `architecture.input_modalities`,
  `supported_parameters`) are ignored, so discovered capabilities are empty.
- **Extension discovery auth is not sanitized (Medium, security).** Builtin
  Bearer and Google hooks strip control bytes; `ext_auth_append`
  (`src/ext/registry.c`) appends the key/prefix verbatim, so a CR/LF in an
  extension-provided key can forge a discovery header line.
- **Ollama fallback / `trim_v1` edges (Low).** The `/models` fallback uses the
  untrimmed base, and `trim_v1` mangles a trailing slash.

## Streams (disjoint file ownership)
- **A (impl): `src/prov/anthropic.c`** — add a sanitized `anthropic_auth_headers`
  discovery hook mirroring `write_headers` (x-api-key for API keys, Bearer for
  `sk-ant-oat`), wire it into `agentc_anthropic_ops.auth_headers`.
- **B (impl): `src/app/main.c`** — run discovery for the selected provider when
  `refresh_models` is set even if the configured model already resolves, keeping
  auto-pick/registration behavior.
- **C (impl): `src/core/discover.c`** — read OpenRouter/gateway metadata
  fallbacks; fix the Ollama fallback URL and `trim_v1` trailing slash.
- **D (impl): `src/ext/registry.c`** — sanitize extension discovery auth
  key/prefix control bytes, parity with builtin hooks.

## Chair
- Owns `tests/discover_test.c`, `tests/data/discover_test.expected`,
  `tests/data/*.mock`, any golden churn, build + `make check` + `./tests/e2e.sh`,
  integration fixes, and the commit.
- After streams land: build, regenerate goldens by hand, run the suite, convene
  two independent reviewers on the diff, fix confirmed findings, re-verify.
