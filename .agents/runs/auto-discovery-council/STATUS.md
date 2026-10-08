# auto-discovery-council — status

Goal: complete "auto model discovery for providers that support it". Recon found
the discovery engine already existed end-to-end; this run closed the gaps that
made auto-discovery wrong or unavailable for providers that *do* support listing.

## Landed
- **Anthropic discovery auth** (`src/prov/anthropic.c`): the row now owns a
  sanitized `auth_headers` hook mirroring the request adapter — `x-api-key` for
  API keys, `authorization: Bearer` + CLI identity headers for `sk-ant-oat`
  subscription tokens. Previously discovery always sent Bearer.
- **`--refresh-models` force path** (`src/app/main.c`): the flag now triggers
  discovery for the selected provider even when the configured model already
  resolves; it was a no-op in that case.
- **Gateway metadata** (`src/core/discover.c`): `parse_openai` reads
  `context_length`/`top_provider.context_length`,
  `top_provider.max_completion_tokens`, `architecture.input_modalities` and the
  input half of `architecture.modality` (an explicit `vision` wins), and a
  `reasoning`/`include_reasoning` entry in `supported_parameters`.
- **Ollama fallback / `trim_v1`** (`src/core/discover.c`): the Ollama fallback is
  `{host}/v1/models` on the trimmed base (no `/v1/v1`), and every trailing slash
  and a `/v1` suffix are trimmed.
- **Extension discovery/request auth sanitizing** (`src/ext/registry.c`): the
  extension auth name/prefix/key have control bytes stripped before the header
  line is written, matching the builtin hooks.
- **Tests + goldens**: new assertions in `tests/discover_test.c` (Anthropic
  API-key vs OAuth headers, OpenRouter metadata + explicit-`vision:false`
  opt-out, Ollama fallback path) and `tests/agent_loop_test.c` (extension key
  CRLF injection). Goldens regenerated — additive only.
- **Docs**: `20-core-agent.md` §3.8 and `30-extensibility.md` §2.7 updated.

## Verification
- `env HOME=/tmp/agentc-ci-home make check` — all 32 golden tests + ext pipeline
  pass, no warnings.
- `env HOME=/tmp/agentc-ci-home ./tests/e2e.sh` — mock provider round-trip,
  `--continue`, pty TUI, onboarding pass.

## Note (pre-existing, not from this change)
- `tests/ext_test` `mem_baseline` fails when run with the real `$HOME` because
  `/home/pvl/.config/agentc/{auth,models-cache,setup}.jsonc` exists; the test
  inherits `HOME` and an OAuth path allocates after the baseline snapshot
  (`src/core/oauth.c` → `store_load`). Reproduced on pristine HEAD; green with a
  clean `HOME`. No code in this change set is involved.
