# TODO — model-discovery

Plan: complete auto model discovery for providers that support listing, plus the interactive setup/onboarding picker.
Status: DONE
Checkpoint: `5e99ca9`
Design contract: `.agents/design/20-core-agent.md` §3.8 (discovery) / §3.9 (auth) / §5.4 (config); `.agents/design/30-extensibility.md` §2.7 (ext auth sanitize); `.agents/design/40-tui.md` §6.4 (setup picker)

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] Anthropic discovery auth mirrors the request adapter — sanitized `x-api-key` for API keys, Bearer + CLI identity for `sk-ant-oat`; the row-owned `auth_headers` hook is authoritative (no Bearer fallback) — provenance: `3a3f5e6`
- [x] `--refresh-models` forces a provider listing even when the configured model already resolves (was a no-op) — provenance: `3a3f5e6`
- [x] OpenAI-shaped parser reads gateway metadata: `context_length`/`top_provider.context_length`, `top_provider.max_completion_tokens`, `architecture.input_modalities` (or input half of `modality`), `supported_parameters` reasoning — provenance: `3a3f5e6`
- [x] Ollama probes native `/api/tags` and falls back to `{host}/v1/models` on the trimmed base; `trim_v1` strips every trailing slash — provenance: `3a3f5e6`
- [x] Extension discovery/request auth name, prefix and key are control-byte sanitized, matching the builtin hooks — provenance: `3a3f5e6`
- [x] Codex/ChatGPT `GET {base}/models` discovery and parser (`slug`/`display_name`/`context_window`/`input_modalities`); `agentc_discover_models_ops` resolves the chosen `openai` row — provenance: `87c58a0` · after: `1e7ecaa`, `8c83a1b`
- [x] `agentc setup`/first-run onboarding use the shared picker (provider, preset, credentials, model) with the numbered line-menu fallback on non-tty — provenance: `87c58a0`
- [x] Codex declares a semver `client_version` capability (default `1.0.0`); the backend hides every model without it — provenance: `1e7ecaa`
- [x] `openai_client_version` is configurable (config / `OPENAI_CLIENT_VERSION` / `AGENTC_CODEX_CLIENT_VERSION`) and published to discovery; Anthropic `/v1/models?limit=1000`; discovery goldens + docs — provenance: `8c83a1b`

## Uncommitted
- [ ] none — the discovery/onboarding units are landed and green on the checkpoint.

## Deferred
- [ ] `src/prov/openai.c` writes the request `api_key` unsanitized; discovery/Anthropic/Google/ext paths sanitize — pre-existing, low-security parity fix not made here
- [ ] Codex `client_version` default `1.0.0` is a capability guess — bump the constant or set `OPENAI_CLIENT_VERSION` if OpenAI raises the bar (§3.8)
