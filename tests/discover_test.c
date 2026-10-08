/* discover_test.c — model discovery, the provider registry and the discovery
cache.
 *
 * Uses the scripted replay backend (src/net/mock.c) so no network is touched.
 */
#include "discover.h"
#include "plat.h"
#include "app/setup.h"
#include "prov/provider.h"
#include "net/net_internal.h"

void agentc_test_setenv(const char *name, const char *value);

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* Bounded byte search: the mock capture buffer is not NUL-terminated. */
static bool sent_contains(const AgcBuf *b, const char *needle) {
    size_t nl = agentc_strlen(needle);
    if (!b || !b->p || nl == 0 || b->len < nl) return false;
    for (size_t i = 0; i + nl <= b->len; i++)
        if (agentc_memeq((const char *)b->p + i, needle, nl)) return true;
    return false;
}

/* Write a one-response mock script (dns + hex-encoded HTTP/1.1 200 body) so a
 * test can synthesise a provider response without adding a data/mock file. */
static void mock_hex(AgcBuf *b, const char *s, size_t n) {
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        agentc_buf_byte(b, (u8)hexd[(u8)s[i] >> 4]);
        agentc_buf_byte(b, (u8)hexd[(u8)s[i] & 0xf]);
    }
}

static int mock_write_json(const char *path, const char *host, const char *json) {
    AgcBuf resp = { 0 };
    agentc_buf_cstr(&resp, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
    agentc_buf_printf(&resp, "%u", (unsigned)agentc_strlen(json));
    agentc_buf_cstr(&resp, "\r\nConnection: close\r\n\r\n");
    agentc_buf_cstr(&resp, json);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns ");
    agentc_buf_cstr(&script, host);
    agentc_buf_cstr(&script, " 203.0.113.9\ndata ");
    mock_hex(&script, (const char *)resp.p, resp.len);
    agentc_buf_byte(&script, '\n');
    int rc = agentc_write_file_atomic(path, script.p, script.len, 0644);
    agentc_buf_free(&resp);
    agentc_buf_free(&script);
    return rc;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* ------------------------------------------------ provider registry */
    const AgcProvider *ollama = agentc_setup_provider("ollama");
    const AgcProvider *cloud = agentc_setup_provider("ollama-cloud");
    const AgcProvider *orouter = agentc_setup_provider("openrouter");
    check("provider_ollama", ollama && agentc_streq(ollama->default_base_url,
                                                "http://127.0.0.1:11434/v1"));
    check("provider_ollama_cloud", cloud && agentc_streq(cloud->default_base_url,
                                                     "https://ollama.com/v1"));
    check("provider_preset", orouter && agentc_streq(orouter->name, "openrouter"));
    check("provider_unknown", agentc_setup_provider("nope") == NULL);
    check("needs_key", !agentc_setup_needs_key("ollama") && agentc_setup_needs_key("openai") &&
                           agentc_setup_needs_key("ollama-cloud"));

    /* OLLAMA_HOST (native host, no /v1) becomes the compatible base URL */
    agentc_test_setenv("OLLAMA_HOST", "http://192.168.1.9:11434");
    check("base_env_ollama",
          agentc_streq(agentc_setup_base_url(NULL, "ollama", NULL), "http://192.168.1.9:11434/v1"));
    agentc_test_setenv("OLLAMA_HOST", "http://host:11434/v1");
    check("base_env_ollama_v1",
          agentc_streq(agentc_setup_base_url(NULL, "ollama", NULL), "http://host:11434/v1"));
    agentc_test_setenv("OLLAMA_HOST", NULL);
    check("base_flag_wins",
          agentc_streq(agentc_setup_base_url(NULL, "openai", "http://flag/v1"), "http://flag/v1"));

    /* a local Ollama request must not carry an Authorization header */
    {
        AgcRequest r;
        agentc_memset(&r, 0, sizeof r);
        r.provider = "ollama";
        r.model = "llama3.2";
        r.transcript = NULL;
        AgcBuf out = { 0 };
        if (ollama->build_request(&out, &r, NULL, "/chat/completions") == 0) {
            const char *head = (const char *)out.p;
            check("ollama_no_auth", agentc_str_str(head, "authorization") == NULL);
            check("ollama_api", agentc_streq(ollama->api, "openai-chat"));
        } else {
            check("ollama_no_auth", false);
            check("ollama_path", false);
        }
        agentc_buf_free(&out);
        agentc_memset(&r, 0, sizeof r);
        r.provider = "ollama-cloud";
        r.model = "gpt-oss:120b";
        r.api_key = "ollama-key";
        AgcBuf out2 = { 0 };
        if (cloud->build_request(&out2, &r, NULL, "/chat/completions") == 0) {
            check("ollama_cloud_bearer",
                  agentc_str_str((const char *)out2.p, "authorization: Bearer ollama-key") != NULL);
        } else {
            check("ollama_cloud_bearer", false);
        }
        agentc_buf_free(&out2);
    }

    /* ------------------------- credential kind drives the openai choice */
    agentc_test_setenv("XDG_CONFIG_HOME", "build/discover.home/.config");
    agentc_test_setenv("HOME", "build/discover.home");
    /* mask any host OPENAI_API_KEY so the OAuth choice is observable */
    agentc_test_setenv("OPENAI_API_KEY", "host-key");
    agentc_test_setenv("OPENAI_API_KEY", NULL);
    /* reset auth.jsonc so a previous run's oauth credential cannot leak in */
    check("choice_auth_reset",
          agentc_write_file_atomic("build/discover.home/.config/agentc/auth.jsonc", "{}\n", 3,
                                   0600) == 0);
    agentc_auth_free();

    const AgcProvider *chat = agentc_prov_openai();
    const AgcProvider *codex = agentc_prov_openai_codex();
    check("choice_explicit_key_chat", agentc_setup_provider_for(NULL, "openai", "sk-x") == chat);
    check("choice_no_cred_chat", agentc_setup_provider_for(NULL, "openai", NULL) == chat);

    const char *auth = "{\"openai\":{\"oauth\":{\"access_token\":\"tok\","
                       "\"expires_at\":9999999999}}}\n";
    check("choice_auth_write",
          agentc_write_file_atomic("build/discover.home/.config/agentc/auth.jsonc", auth,
                                   agentc_strlen(auth), 0600) == 0);
    agentc_auth_free();
    check("choice_oauth_codex", agentc_setup_provider_for(NULL, "openai", NULL) == codex);
    check("choice_explicit_beats_oauth",
          agentc_setup_provider_for(NULL, "openai", "sk-x") == chat);
    agentc_setup_set_context(NULL, "sk-ctx", NULL);
    check("choice_context_chat", agentc_setup_provider("openai") == chat);
    agentc_setup_set_context(NULL, NULL, NULL);

    /* an environment key counts as explicit and beats the stored OAuth token */
    agentc_test_setenv("OPENAI_API_KEY", "env-key");
    agentc_auth_free();
    check("choice_env_chat", agentc_setup_provider_for(NULL, "openai", NULL) == chat);
    agentc_test_setenv("OPENAI_API_KEY", "host-key");
    agentc_test_setenv("OPENAI_API_KEY", NULL);

    /* the api filter keeps the two openai wires apart */
    const AgcModel *chat_models[64];
    size_t cm = agentc_model_filter("openai", "openai-chat", chat_models, 64);
    bool saw_gpt5 = false, saw_codex = false;
    for (size_t i = 0; i < cm; i++) {
        if (agentc_streq(chat_models[i]->id, "gpt-5")) saw_gpt5 = true;
        if (agentc_streq(chat_models[i]->api, "openai-codex-responses")) saw_codex = true;
    }
    check("model_filter_chat", cm >= 3 && saw_gpt5 && !saw_codex);
    const AgcModel *codex_models[64];
    size_t xm = agentc_model_filter("openai", "openai-codex-responses", codex_models, 64);
    bool codex_wrong_api = false;
    for (size_t i = 0; i < xm; i++)
        if (!agentc_streq(codex_models[i]->api, "openai-codex-responses")) codex_wrong_api = true;
    check("model_filter_codex", xm >= 2 && !codex_wrong_api);
    agentc_auth_free();

    /* -------------------------------------------------- discovery parsing */
    check("mock_openai_load", agentc_mock_load("tests/data/discover_openai.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("openai", "http://api.test/v1", "sk-test", &m, 16, 3000,
                                      err, sizeof err);
        check("openai_count", n == 2);
        check("openai_ids", n == 2 && agentc_streq(m[0].id, "gpt-x") && agentc_streq(m[1].id, "gpt-y"));
        check("openai_extras", n == 2 && m[1].ctx_window == 128000 && m[1].max_tokens == 8192 &&
                                   m[1].image);
        agentc_discover_free(m, n);
    }

    /* A key is untrusted input: a CR/LF in it must not forge an extra header
     * line in the discovery request. */
    check("mock_key_inject_load", agentc_mock_load("tests/data/discover_openai.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("openai", "http://api.test/v1", "sk-test\r\nX: y",
                                      &m, 16, 3000, err, sizeof err);
        const AgcBuf *sent = agentc_mock_sent();
        check("discover_key_sanitized",
              n == 2 && !sent_contains(sent, "\r\nX: y") &&
                  sent_contains(sent, "authorization: Bearer sk-testX: y\r\n"));
        agentc_discover_free(m, n);
    }

    check("mock_ollama_load", agentc_mock_load("tests/data/discover_ollama.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("ollama", "http://api.test:11434/v1", NULL, &m, 16, 3000,
                                      err, sizeof err);
        check("ollama_count", n == 2);
        check("ollama_ids",
              n == 2 && agentc_streq(m[0].id, "llama3.2:latest") &&
                  agentc_streq(m[1].id, "qwen2.5-coder:7b"));
        check("ollama_details", n == 2 && agentc_str_str(m[0].detail, "3.2B") != NULL &&
                                    agentc_str_str(m[1].detail, "qwen2") != NULL);
        agentc_discover_free(m, n);
    }

    /* Native /api/tags reports no models: the OpenAI-compatible fallback must
     * target {host}/v1/models on the trimmed base, even when the supplied base
     * has no /v1. */
    check("mock_ollama_fallback_write",
          mock_write_json("build/discover.home/ollama-fallback.mock", "api.test",
                          "{\"models\":[]}\n") == 0);
    check("mock_ollama_fallback_load",
          agentc_mock_load("build/discover.home/ollama-fallback.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("ollama", "http://api.test:11434", NULL, &m, 16,
                                          3000, err, sizeof err);
        const AgcBuf *sent = agentc_mock_sent();
        check("ollama_fallback_path",
              n == 0 && sent_contains(sent, "GET /v1/models") &&
                  !sent_contains(sent, "/v1/v1"));
        agentc_discover_free(m, n);
    }

    check("mock_anthropic_load", agentc_mock_load("tests/data/discover_anthropic.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("anthropic", "http://api.test", "sk-ant-test", &m, 16,
                                      3000, err, sizeof err);
        check("anthropic_count", n == 1);
        check("anthropic_name", n == 1 && agentc_streq(m[0].id, "claude-x") &&
                                    agentc_streq(m[0].name, "Claude X"));
        /* An API key must reach discovery as x-api-key, exactly like a request,
         * never as a Bearer token (the request adapter's rule). */
        const AgcBuf *sent = agentc_mock_sent();
        check("anthropic_api_key_header",
              sent_contains(sent, "x-api-key: sk-ant-test\r\n") &&
                  !sent_contains(sent, "authorization: Bearer"));
        agentc_discover_free(m, n);
    }
    /* A Claude subscription token (sk-ant-oat…) must reach discovery as a Bearer
     * token, exactly like a request, and not as x-api-key. */
    check("mock_anthropic_oauth_load",
          agentc_mock_load("tests/data/discover_anthropic.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("anthropic", "http://api.test", "sk-ant-oat-xyz", &m,
                                      16, 3000, err, sizeof err);
        const AgcBuf *sent = agentc_mock_sent();
        check("anthropic_oauth_bearer",
              n == 1 && sent_contains(sent, "authorization: Bearer sk-ant-oat-xyz\r\n") &&
                  sent_contains(sent, "anthropic-beta: oauth-2025-04-20") &&
                  !sent_contains(sent, "x-api-key:"));
        agentc_discover_free(m, n);
    }

    /* Native Gemini list: the response wraps models under "models" and prefixes
     * each name with "models/"; the id must be stripped, and the request must
     * carry x-goog-api-key instead of a Bearer token. */
    check("mock_google_load", agentc_mock_load("tests/data/discover_google.mock") == 0);
    {
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("google", "http://api.test/v1beta", "sk-google", &m,
                                      16, 3000, err, sizeof err);
        const AgcBuf *sent = agentc_mock_sent();
        check("google_count", n == 2);
        check("google_ids", n == 2 && agentc_streq(m[0].id, "gemini-2.5-flash") &&
                                 agentc_streq(m[1].id, "gemini-2.5-pro"));
        check("google_names", n == 2 && agentc_streq(m[0].name, "Gemini 2.5 Flash"));
        check("google_limits", n == 2 && m[0].ctx_window == 1048576 &&
                                   m[0].max_tokens == 65536 && m[0].reasoning);
        check("google_path", sent_contains(sent, "GET /v1beta/models"));
        check("google_auth_header",
              sent_contains(sent, "x-goog-api-key: sk-google\r\n") &&
                  !sent_contains(sent, "authorization: Bearer"));
        agentc_discover_free(m, n);
    }

    /* Codex/ChatGPT backend list: the id is `slug`, hidden rows are kept and a
     * row the backend marks not API-supported is skipped. The ops-driven entry
     * resolves the codex wire (the name "openai" also maps to chat). */
    {
        const char *cjson =
            "{\"models\":["
            "{\"slug\":\"gpt-5.6-sol\",\"display_name\":\"GPT-5.6-Sol\","
            "\"default_reasoning_level\":\"medium\","
            "\"supported_reasoning_levels\":[{\"effort\":\"low\"}]},"
            "{\"slug\":\"gpt-5.5\",\"display_name\":\"GPT-5.5\",\"visibility\":\"hide\"},"
            "{\"slug\":\"no-api\",\"supported_in_api\":false}"
            "]}\n";
        check("codex_mock_write",
              mock_write_json("build/discover.home/codex.mock", "api.test", cjson) == 0);
        check("codex_mock_load", agentc_mock_load("build/discover.home/codex.mock") == 0);
        const AgcProviderOps *codex = agentc_provider_ops(agentc_prov_openai_codex());
        check("codex_row", codex != NULL && codex->discover_style == AGENTC_DISCOVER_CODEX);
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models_ops(codex, "http://api.test/v1", "oauth-token", &m, 16,
                                              3000, err, sizeof err);
        check("codex_count", n == 2);
        check("codex_ids", n == 2 && agentc_streq(m[0].id, "gpt-5.6-sol") &&
                               agentc_streq(m[0].name, "GPT-5.6-Sol") && m[0].reasoning &&
                               agentc_streq(m[1].id, "gpt-5.5"));
        agentc_discover_free(m, n);
    }

    /* OpenRouter shape: pricing (USD per token strings) must populate the
     * registered model's rates. The parse path is exercised with no network. */
    {
        const char *orjson =
            "{\"data\":["
            "{\"id\":\"vendor/model-a\",\"name\":\"Model A\","
            "\"context_length\":200000,"
            "\"architecture\":{\"input_modalities\":[\"text\",\"image\"]},"
            "\"supported_parameters\":[\"tools\",\"reasoning\"],"
            "\"pricing\":{\"prompt\":\"0.0000005\",\"completion\":\"0.0000015\","
            "\"input_cache_read\":\"0.00000005\"}},"
            "{\"id\":\"vendor/free\",\"name\":\"Free\","
            "\"top_provider\":{\"context_length\":131072,\"max_completion_tokens\":4096},"
            "\"architecture\":{\"modality\":\"text+image->text\"},"
            "\"supported_parameters\":[\"include_reasoning\"],"
            "\"pricing\":{\"prompt\":\"0\",\"completion\":\"0\"}},"
            "{\"id\":\"vendor/vision-off\",\"name\":\"No Vision\",\"vision\":false,"
            "\"architecture\":{\"input_modalities\":[\"text\",\"image\"]}}"
            "]}\n";
        check("openrouter_mock_write",
              mock_write_json("build/discover.home/openrouter.mock", "or.test", orjson) == 0);
        check("openrouter_mock_load",
              agentc_mock_load("build/discover.home/openrouter.mock") == 0);
        AgcDiscovered *m = NULL;
        char err[128] = "";
        size_t n = agentc_discover_models("openrouter", "http://or.test/v1", NULL, &m, 16,
                                          3000, err, sizeof err);
        check("openrouter_count", n == 3);
        /* gateway metadata: direct context_length + input_modalities + a
         * supported_parameters reasoning flag */
        check("openrouter_meta_direct",
              n == 3 && m[0].ctx_window == 200000 && m[0].image && m[0].reasoning);
        /* fallbacks: top_provider context/max, the input half of
         * architecture.modality, and the include_reasoning spelling */
        check("openrouter_meta_fallback",
              n == 3 && m[1].ctx_window == 131072 && m[1].max_tokens == 4096 &&
                  m[1].image && m[1].reasoning);
        /* an explicit vision:false wins over the declared input modalities */
        check("openrouter_meta_optout", n == 3 && !m[2].image);
        agentc_discover_register("openrouter", "openai-chat", "http://or.test/v1", m, n);
        agentc_discover_free(m, n);
        const AgcModel *a = agentc_model_find("openrouter", "vendor/model-a");
        check("openrouter_rate_known",
              a && (a->rate_flags & AGENTC_MODEL_RATE_KNOWN) && a->in_rate == 500000 &&
                  a->out_rate == 1500000 && a->cache_read_rate == 50000 &&
                  !(a->rate_flags & AGENTC_MODEL_INPUT_INCLUDES_CACHE));
        AgcUsage u;
        agentc_memset(&u, 0, sizeof u);
        u.input = 1000;
        u.output = 500;
        u.cache_read = 200;
        check("openrouter_cost", a && agentc_model_cost(a, &u) == 1260);
        const AgcModel *f = agentc_model_find("openrouter", "vendor/free");
        check("openrouter_zero_skipped",
              f && !(f->rate_flags & AGENTC_MODEL_RATE_KNOWN) && agentc_model_cost(f, &u) == -1);
        agentc_model_clear_dynamic("openrouter");
    }

    /* On-demand OpenRouter sync (no network): the mock transport serves the
     * public models payload, which must be merged into pricing.jsonc and
     * applied. */
    {
        const char *orjson =
            "{\"data\":["
            "{\"id\":\"vendor/model-a\",\"name\":\"Model A\","
            "\"pricing\":{\"prompt\":\"0.0000005\",\"completion\":\"0.0000015\","
            "\"input_cache_read\":\"0.00000005\"}}"
            "]}\n";
        check("sync_mock_write",
              mock_write_json("build/discover.home/sync.mock", "openrouter.ai", orjson) == 0);
        check("sync_mock_load", agentc_mock_load("build/discover.home/sync.mock") == 0);
        agentc_model_clear_dynamic("openrouter");
        char serr[128] = "";
        check("sync_count", agentc_pricing_sync_openrouter(serr, sizeof serr) == 1);
        agentc_model_register_dynamic("openrouter", "vendor/model-a", "openai-chat",
                                      "http://or.test/v1", 0, 0, false, false);
        const AgcModel *sa = agentc_model_find("openrouter", "vendor/model-a");
        AgcUsage su;
        agentc_memset(&su, 0, sizeof su);
        su.input = 1000;
        su.output = 500;
        su.cache_read = 200;
        check("sync_applied", sa && agentc_model_cost(sa, &su) == 1260);
        size_t plen = 0;
        char *ptext = agentc_read_file_owned("build/discover.home/.config/agentc/pricing.jsonc",
                                             &plen);
        check("sync_file_merged",
              ptext && agentc_str_str(ptext, "openrouter/vendor/model-a") != NULL);
        agentc_free(ptext);
        agentc_model_clear_dynamic("openrouter");
    }

    /* ------------------------------------------------ cache + registration */
    agentc_test_setenv("XDG_CONFIG_HOME", "build/discover.home/.config");
    agentc_test_setenv("HOME", "build/discover.home");
    {
        /* agentc_discover_free releases the array itself, so it must be heap-owned */
        AgcDiscovered *m = agentc_alloc(2 * sizeof *m);
        agentc_memset(m, 0, 2 * sizeof *m);
        m[0].id = agentc_strdup("cached-a");
        m[0].name = agentc_strdup("cached-a");
        m[1].id = agentc_strdup("cached-b");
        m[1].name = agentc_strdup("cached-b");
        m[1].detail = agentc_strdup("7B");
        m[1].ctx_window = 4096;
        check("cache_save", agentc_discover_cache_save("ollama", "http://127.0.0.1:11434/v1", m, 2) == 0);
        agentc_discover_free(m, 2);

        AgcDiscovered *loaded = NULL;
        i64 fetched = 0;
        size_t n = agentc_discover_cache_load("ollama", "http://127.0.0.1:11434/v1", &loaded, 16,
                                          &fetched);
        check("cache_load", n == 2 && agentc_streq(loaded[1].id, "cached-b") &&
                                loaded[1].ctx_window == 4096 &&
                                agentc_str_str(loaded[1].detail, "7B") != NULL && fetched > 0);
        agentc_discover_free(loaded, n);

        /* a cache written for another endpoint is a miss */
        AgcDiscovered *mismatch = NULL;
        size_t mn = agentc_discover_cache_load("ollama", "http://other.invalid/v1", &mismatch, 16,
                                           NULL);
        check("cache_base_mismatch", mn == 0 && mismatch == NULL);
        agentc_discover_free(mismatch, mn);

        /* registering makes the model visible to the agent's catalog */
        check("dynamic_missing_before", agentc_model_find("ollama", "cached-b") == NULL);
        AgcDiscovered one;
        agentc_memset(&one, 0, sizeof one);
        one.id = "cached-b";
        one.name = "cached-b";
        agentc_discover_register("ollama", "openai-chat", "http://127.0.0.1:11434/v1", &one, 1);
        const AgcModel *found = agentc_model_find("ollama", "cached-b");
        check("dynamic_registered", found != NULL && agentc_model_is_dynamic(found) &&
                                        agentc_model_dynamic_count() == 1);
        agentc_model_clear_dynamic("ollama");
        check("dynamic_cleared",
              agentc_model_find("ollama", "cached-b") == NULL && agentc_model_dynamic_count() == 0);

        /* a second provider's cache entry must be preserved by the merge */
        AgcDiscovered other;
        agentc_memset(&other, 0, sizeof other);
        other.id = "openai-model";
        other.name = "openai-model";
        check("cache_merge",
              agentc_discover_cache_save("openai", "https://api.openai.com/v1", &other, 1) == 0);
        n = agentc_discover_cache_load("ollama", "http://127.0.0.1:11434/v1", &loaded, 16, NULL);
        check("cache_merge_kept", n == 2);
        agentc_discover_free(loaded, n);

        /* a failed live refresh must still report the cached count so main.c
         * does not skip auto-selection and error "no model selected" */
        check("refresh_fail_mock", agentc_mock_load("tests/data/provider_http_400.mock") == 0);
        agentc_test_setenv("OLLAMA_HOST", "http://127.0.0.1:11434/v1");
        agentc_model_clear_dynamic("ollama");
        size_t kept = agentc_setup_discover(NULL, "ollama", true, false, true);
        check("refresh_fail_keeps_cache",
              kept == 2 && agentc_model_find("ollama", "cached-b") != NULL);
        agentc_test_setenv("OLLAMA_HOST", NULL);
        agentc_model_clear_dynamic("ollama");
    }

    /* a successful refresh replaces the cache set: a model the endpoint no
     * longer reports must not stay selectable later in the run */
    {
        AgcDiscovered stale;
        agentc_memset(&stale, 0, sizeof stale);
        stale.id = "stale-x";
        stale.name = "stale-x";
        check("refresh_stale_save",
              agentc_discover_cache_save("openai", "http://api.test/v1", &stale, 1) == 0);
        check("refresh_stale_mock", agentc_mock_load("tests/data/discover_openai.mock") == 0);
        agentc_test_setenv("OPENAI_API_KEY", "test-key");
        agentc_test_setenv("OPENAI_BASE_URL", "http://api.test/v1");
        agentc_model_clear_dynamic("openai");
        size_t got = agentc_setup_discover(NULL, "openai", true, false, true);
        check("refresh_drops_stale",
              got >= 2 && agentc_model_find("openai", "stale-x") == NULL &&
                  agentc_model_find("openai", "gpt-x") != NULL);
        agentc_test_setenv("OPENAI_API_KEY", NULL);
        agentc_test_setenv("OPENAI_BASE_URL", NULL);
        agentc_model_clear_dynamic("openai");
    }

    check("no_errors", fails == 0);
    return fails;
}
