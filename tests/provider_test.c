/* provider_test.c — canned SSE bytes through map_sse, asserted against the
 * decoded assistant message; request builders; the real HTTP transport through
 * the scripted mock backend (wire-level golden for ollama-cloud, plus the
 * non-2xx error excerpt).
 */
#include "agent.h"
#include "net/net_internal.h"
#include "prov/provider.h"
#include "app/setup.h"
#include "config.h"

/* internal test hook from agent.c */
void agentc_agent_test_no_backoff(AgcAgent *a);
/* internal helper from messages.c */
void agentc_msg_add_tool_result(AgcMsg *m, const char *call_id, const char *name,
                            const char *result);
/* internal parser from transport_http.c (unit-tested here) */
i64 agentc_transport_parse_retry_after(const char *v);

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

typedef struct {
    const AgcProvider *p;
    AgcStreamState *st;
} MapCtx;

static int on_ev(void *ud, const AgcSseEvent *ev) {
    MapCtx *c = ud;
    if (c->p->map_sse(c->st, ev) != 0) return 0; /* recorded via st->error */
    return 0;
}

static void stop(AgcStreamState *st) { agentc_stream_state_close(st); }

static void start(AgcTranscript *tr, AgcMsg **m, AgcStreamState *st, MapCtx *c,
                  const AgcProvider *p) {
    agentc_transcript_init(tr);
    *m = agentc_transcript_push(tr, AGENTC_ROLE_ASSISTANT);
    (void)agentc_stream_state_init(p, st);   /* zeroes st; stop() balances it */
    st->msg = *m;
    c->p = p;
    c->st = st;
}

static void feed(AgcSse *sse, MapCtx *c, const char *bytes) {
    agentc_sse_feed(sse, bytes, agentc_strlen(bytes), on_ev, c);
}

static bool block_is(const AgcMsg *m, size_t i, int type, const char *text) {
    if (i >= m->nblocks) return false;
    const AgcBlock *b = &m->blocks[i];
    if (b->type != type) return false;
    size_t n = text ? agentc_strlen(text) : 0;
    return agentc_str_eq(b->text ? b->text : "", b->text_len, text ? text : "", n);
}

/* ------------------------------------------- mock transport helpers */
static bool sent_has(const char *needle) {
    const AgcBuf *s = agentc_mock_sent();
    return s != NULL && s->p != NULL &&
           agentc_str_find((const char *)s->p, s->len, needle, agentc_strlen(needle)) >= 0;
}

/* Copy the JSON body of the recorded request (after the blank line). */
static bool sent_body(AgcBuf *out) {
    const AgcBuf *s = agentc_mock_sent();
    if (s == NULL || s->p == NULL) return false;
    i64 k = agentc_str_find((const char *)s->p, s->len, "\r\n\r\n", 4);
    if (k < 0) return false;
    agentc_buf_push(out, s->p + k + 4, s->len - (size_t)k - 4);
    return true;
}

/* --------------------------------------- empty assistant staging slot */
/* The agent loop keeps an empty assistant message as the in-progress slot for
 * the current turn. Serialising it produced `"content": null` with no
 * tool_calls, which Ollama's OpenAI layer rejects with HTTP 400
 * ("invalid message content type: <nil>"). It must never reach the wire. */
static void test_empty_assistant_skipped(void) {
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    AgcMsg *um = agentc_transcript_push(&tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(um, "hi", 2);
    (void)agentc_transcript_push(&tr, AGENTC_ROLE_ASSISTANT); /* empty slot */

    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    r.provider = "ollama-cloud";
    r.model = "gpt-oss:120b";
    r.system = "sys";
    r.transcript = &tr;

    AgcBuf out = { 0 };
    int rc = agentc_prov_ollama_cloud()->build_request(&out, &r, NULL, "/chat/completions");
    i64 k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    bool ok = rc == 0 && k > 0;
    if (ok) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        AgcJson *msgs = agentc_json_get(body, "messages");
        /* system + user; the empty assistant staging slot is dropped */
        ok = agentc_json_len(msgs) == 2 &&
             agentc_json_is(agentc_json_get(agentc_json_at(msgs, 0), "role"), "system") &&
             agentc_json_is(agentc_json_get(agentc_json_at(msgs, 1), "role"), "user");
    }
    check("empty_assistant_skipped_openai", ok);
    agentc_buf_free(&out);

    /* Anthropic would otherwise receive an empty content array */
    agentc_memset(&r, 0, sizeof r);
    r.provider = "anthropic";
    r.model = "claude-sonnet-4-5";
    r.system = "sys";
    r.transcript = &tr;
    out = (AgcBuf){ 0 };
    rc = agentc_prov_anthropic()->build_request(&out, &r, NULL, "/v1/messages");
    k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    ok = rc == 0 && k > 0;
    if (ok) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        ok = agentc_json_len(agentc_json_get(body, "messages")) == 1;
    }
    check("empty_assistant_skipped_anthropic", ok);
    agentc_buf_free(&out);
    agentc_transcript_free(&tr);
}

/* ---------------------------- max token field per provider */
/* Ollama's OpenAI layer ignores max_completion_tokens (verified: a cap of 10
 * still produced 898 completion tokens) while first-party OpenAI reasoning
 * models require it, so the field name is chosen by provider. */
static void test_max_tokens_field(void) {
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    AgcMsg *um = agentc_transcript_push(&tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(um, "hi", 2);

    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    r.model = "m";
    r.system = "sys";
    r.transcript = &tr;
    r.max_tokens = 32;

    r.provider = "openai";
    AgcBuf out = { 0 };
    agentc_prov_openai()->build_request(&out, &r, NULL, "/chat/completions");
    i64 k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    bool ok = k > 0;
    if (ok) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        ok = agentc_json_get_int(body, "max_completion_tokens", -1) == 32 &&
             agentc_json_get(body, "max_tokens") == NULL;
    }
    check("max_tokens_field_openai", ok);
    agentc_buf_free(&out);

    r.provider = "ollama-cloud";
    out = (AgcBuf){ 0 };
    agentc_prov_ollama_cloud()->build_request(&out, &r, NULL, "/chat/completions");
    k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    ok = k > 0;
    if (ok) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        ok = agentc_json_get_int(body, "max_tokens", -1) == 32 &&
             agentc_json_get(body, "max_completion_tokens") == NULL;
    }
    check("max_tokens_field_ollama", ok);
    agentc_buf_free(&out);

    /* every OpenAI-compatible preset uses max_tokens, not just ollama */
    const AgcProviderOps *preset = agentc_provider_by_name("openrouter");
    r.provider = "openrouter";
    out = (AgcBuf){ 0 };
    if (preset) preset->build_request(&out, &r, NULL, "/chat/completions");
    k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    ok = preset && k > 0;
    if (ok) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        ok = agentc_json_get_int(body, "max_tokens", -1) == 32 &&
             agentc_json_get(body, "max_completion_tokens") == NULL;
    }
    check("max_tokens_field_preset", ok);
    agentc_buf_free(&out);

    /* a user endpoint defaults to the broadly compatible max_tokens too */
    const AgcProvider *custom = agentc_prov_openai_compatible("reg-max-custom", "http://reg-max/v1");
    r.provider = "reg-max-custom";
    out = (AgcBuf){ 0 };
    if (custom) custom->build_request(&out, &r, NULL, "/chat/completions");
    k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    ok = custom && k > 0;
    if (ok) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        ok = agentc_json_get_int(body, "max_tokens", -1) == 32 &&
             agentc_json_get(body, "max_completion_tokens") == NULL;
    }
    check("max_tokens_field_custom", ok);
    agentc_buf_free(&out);
    agentc_transcript_free(&tr);
}

/* Registered-but-hidden tools (AGENTC_TOOL_HIDDEN) execute but must never be
 * declared to the model, on any wire. */
static bool body_tools_have(const AgcBuf *out, const char *name, size_t *count) {
    i64 k = agentc_str_find((const char *)out->p, out->len, "\r\n\r\n", 4);
    if (k <= 0) return false;
    AgcJson *body = agentc_json_parse((const char *)out->p + k + 4, out->len - (size_t)k - 4);
    AgcJson *tools = agentc_json_get(body, "tools");
    size_t n = agentc_json_len(tools);
    if (count) *count = n;
    for (size_t i = 0; i < n; i++) {
        AgcJson *t = agentc_json_at(tools, i);
        const char *tn = agentc_json_get_str(t, "name");
        if (!tn) tn = agentc_json_get_str(agentc_json_get(t, "function"), "name");
        if (tn && agentc_streq(tn, name)) return true;
    }
    return false;
}

static void test_hidden_tools_skipped(void) {
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    AgcMsg *um = agentc_transcript_push(&tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(um, "hi", 2);

    AgcTool tools[2];
    size_t nt = agentc_tools_builtin(tools, 2);
    bool ok = nt == 2;
    if (ok) tools[1].flags |= AGENTC_TOOL_HIDDEN;
    const char *hidden = ok ? tools[1].name : "";
    const char *visible = ok ? tools[0].name : "";

    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    r.model = "m";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 2;

    AgcBuf out = { 0 };
    r.provider = "openai";
    ok = ok && agentc_prov_openai()->build_request(&out, &r, NULL, "/chat/completions") == 0;
    size_t count = 0;
    bool has_hidden = body_tools_have(&out, hidden, &count);
    bool has_visible = body_tools_have(&out, visible, NULL);
    check("hidden_tools_openai", ok && count == 1 && has_visible && !has_hidden);
    agentc_buf_free(&out);

    agentc_memset(&r, 0, sizeof r);
    r.provider = "anthropic";
    r.model = "claude-sonnet-4-5";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 2;
    out = (AgcBuf){ 0 };
    ok = agentc_prov_anthropic()->build_request(&out, &r, NULL, "/v1/messages") == 0;
    has_hidden = body_tools_have(&out, hidden, &count);
    has_visible = body_tools_have(&out, visible, NULL);
    check("hidden_tools_anthropic", ok && count == 1 && has_visible && !has_hidden);
    agentc_buf_free(&out);

    agentc_memset(&r, 0, sizeof r);
    r.provider = "openai";
    r.model = "gpt-5-codex";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 2;
    out = (AgcBuf){ 0 };
    ok = agentc_prov_openai_codex()->build_request(&out, &r, NULL, "/responses") == 0;
    has_hidden = body_tools_have(&out, hidden, &count);
    has_visible = body_tools_have(&out, visible, NULL);
    check("hidden_tools_codex", ok && count == 1 && has_visible && !has_hidden);
    agentc_buf_free(&out);

    agentc_transcript_free(&tr);
}

/* -------------------------------------- ollama-cloud wire golden */
static void test_ollama_cloud_wire(void) {
    check("oc_mock_load", agentc_mock_load("tests/data/provider_ollama_cloud.mock") == 0);

    AgcTool tools[1];
    agentc_tools_builtin(tools, 1);

    AgcAgent *a = agentc_agent_new(agentc_prov_ollama_cloud(), "gpt-oss:120b");
    agentc_agent_set_transport(a, agentc_transport_http(false));
    agentc_agent_set_api_key(a, "oc-key-123");
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, tools, 1);
    agentc_agent_set_max_tokens(a, 64);
    agentc_agent_set_retry(a, 1);
    agentc_agent_test_no_backoff(a);

    int rc = agentc_agent_submit(a, "hello");
    check("oc_rc", rc == 0);
    check("oc_url", sent_has("POST /v1/chat/completions HTTP/1.1\r\n"));
    check("oc_host", sent_has("Host: ollama.com\r\n"));
    check("oc_auth", sent_has("authorization: Bearer oc-key-123\r\n"));
    check("oc_accept", sent_has("accept: text/event-stream\r\n"));

    AgcBuf body = { 0 };
    bool have = sent_body(&body);
    check("oc_body", have);
    if (have) {
        AgcJson *j = agentc_json_parse((const char *)body.p, body.len);
        AgcJson *msgs = agentc_json_get(j, "messages");
        AgcJson *so = agentc_json_get(j, "stream_options");
        check("oc_json_model", agentc_json_is(agentc_json_get(j, "model"), "gpt-oss:120b"));
        check("oc_json_stream",
              agentc_json_get_bool(j, "stream", false) &&
                  agentc_json_get_bool(so, "include_usage", false));
        /* system + user only: the empty in-progress assistant is not sent */
        check("oc_json_messages", agentc_json_len(msgs) == 2);
        check("oc_json_tools", agentc_json_len(agentc_json_get(j, "tools")) == 1);
        /* Ollama reads max_tokens (num_predict); max_completion_tokens is ignored */
        check("oc_json_max_tokens",
              agentc_json_get_int(j, "max_tokens", -1) == 64 &&
                  agentc_json_get(j, "max_completion_tokens") == NULL);
        check("oc_json_no_unsupported",
              agentc_json_get(j, "tool_choice") == NULL && agentc_json_get(j, "options") == NULL &&
                  agentc_json_get(j, "think") == NULL && agentc_json_get(j, "num_ctx") == NULL &&
                  agentc_json_get(j, "reasoning_effort") == NULL);
    } else {
        check("oc_json_model", false);
        check("oc_json_stream", false);
        check("oc_json_messages", false);
        check("oc_json_tools", false);
        check("oc_json_max_tokens", false);
        check("oc_json_no_unsupported", false);
    }
    agentc_buf_free(&body);
    agentc_agent_free(a);
}

/* -------------------------------------- native Gemini wire golden */
static void test_google_wire(void) {
    check("g_mock_load", agentc_mock_load("tests/data/provider_google.mock") == 0);

    AgcTool tools[1];
    agentc_tools_builtin(tools, 1);

    AgcAgent *a = agentc_agent_new(agentc_prov_google(), "gemini-2.5-flash");
    agentc_agent_set_transport(a, agentc_transport_http(false));
    agentc_agent_set_api_key(a, "goog-key-123");
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, tools, 1);
    agentc_agent_set_max_tokens(a, 64);
    agentc_agent_set_retry(a, 1);
    agentc_agent_test_no_backoff(a);

    int rc = agentc_agent_submit(a, "hello");
    check("g_rc", rc == 0);
    /* the model is substituted into the row path, with alt=sse for streaming */
    check("g_url", sent_has("POST /v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse "
                            "HTTP/1.1\r\n"));
    check("g_host", sent_has("Host: generativelanguage.googleapis.com\r\n"));
    check("g_auth", sent_has("x-goog-api-key: goog-key-123\r\n"));
    check("g_accept", sent_has("accept: text/event-stream\r\n"));

    AgcBuf body = { 0 };
    bool have = sent_body(&body);
    check("g_body", have);
    if (have) {
        AgcJson *j = agentc_json_parse((const char *)body.p, body.len);
        AgcJson *si = agentc_json_get(j, "systemInstruction");
        AgcJson *contents = agentc_json_get(j, "contents");
        AgcJson *gen = agentc_json_get(j, "generationConfig");
        check("g_json_system",
              agentc_json_is(agentc_json_get(agentc_json_at(agentc_json_get(si, "parts"), 0),
                                              "text"),
                             "sys"));
        check("g_json_contents", agentc_json_len(contents) == 1 &&
                                     agentc_json_is(agentc_json_get(agentc_json_at(contents, 0),
                                                                    "role"),
                                                    "user"));
        check("g_json_tools", agentc_json_len(agentc_json_get(j, "tools")) == 1);
        check("g_json_max_tokens", agentc_json_get_int(gen, "maxOutputTokens", -1) == 64);
        check("g_json_no_unsupported",
              agentc_json_get(j, "model") == NULL && agentc_json_get(j, "stream") == NULL);
    } else {
        check("g_json_system", false);
        check("g_json_contents", false);
        check("g_json_tools", false);
        check("g_json_max_tokens", false);
        check("g_json_no_unsupported", false);
    }
    agentc_buf_free(&body);
    agentc_agent_free(a);
}

/* ------------------------------------ non-2xx body surfacing */
static void test_http_error_excerpt(void) {
    check("err_mock_load", agentc_mock_load("tests/data/provider_http_400.mock") == 0);

    AgcAgent *a = agentc_agent_new(agentc_prov_ollama_cloud(), "gpt-oss:120b");
    agentc_agent_set_transport(a, agentc_transport_http(false));
    agentc_agent_set_api_key(a, "oc-key-123");
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 1);
    agentc_agent_test_no_backoff(a);

    int rc = agentc_agent_submit(a, "hello");
    check("err_rc", rc == -1);
    const char *err = agentc_agent_last_error(a);
    check("err_status", err != NULL && agentc_str_str(err, "http error 400") != NULL);
    check("err_provider", err != NULL && agentc_str_str(err, "ollama-cloud") != NULL);
    check("err_url",
          err != NULL && agentc_str_str(err, "https://ollama.com/v1/chat/completions") != NULL);
    check("err_body", err != NULL && agentc_str_str(err, "bad key") != NULL &&
                          agentc_str_str(err, "line two") != NULL);
    check("err_redacted",
          err != NULL && agentc_str_str(err, "oc-key-123") == NULL &&
              agentc_str_str(err, "oc-") == NULL);
    check("err_single_line",
          err != NULL && agentc_str_str(err, "\n") == NULL && agentc_str_str(err, "\r") == NULL &&
              agentc_str_str(err, "\t") == NULL);
    const char *ex = agentc_transport_http_last_body_excerpt();
    check("err_excerpt_bound", ex != NULL && agentc_strlen(ex) <= 512 && ex[0] != 0);
    /* the second key copy sits at byte 507: straddling the 512-byte cut must not
     * leave a partial credential behind (redaction happens before truncation) */
    check("err_excerpt_redacted",
          ex != NULL && agentc_str_str(ex, "oc-key-123") == NULL &&
              agentc_str_str(ex, "oc-") == NULL);
    agentc_agent_free(a);
}

/* ---------------------------------------------------------------- codex */

static void test_codex_text(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai_codex();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "event: response.created\n"
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"message\"}}\n\n"
         "event: response.output_text.delta\n"
         "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hi \"}\n\n"
         "data: {\"type\":\"response.output_text.delta\",\"delta\":\"there\"}\n\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\","
         "\"usage\":{\"input_tokens\":11,\"output_tokens\":4,"
         "\"input_tokens_details\":{\"cached_tokens\":3},"
         "\"output_tokens_details\":{\"reasoning_tokens\":1}}}}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("codex_text_blocks", m->nblocks == 1);
    check("codex_text_body", block_is(m, 0, AGENTC_BLK_TEXT, "Hi there"));
    check("codex_text_usage",
          st.usage_input == 11 && st.usage_output == 4 && st.usage_cache_read == 3 &&
              st.usage_reasoning == 1);
    check("codex_text_stop", st.stop_reason == AGENTC_STOP_STOP && st.saw_stop);
    check("codex_text_id", agentc_streq(st.response_id, "resp_1"));
    check("codex_text_api", agentc_streq(p->api, "openai-codex-responses") &&
                                agentc_streq(p->default_base_url,
                                             "https://chatgpt.com/backend-api/codex"));
    agentc_transcript_free(&tr);
}

/* `response.incomplete` ends a truncated turn: max_output_tokens maps to LENGTH
 * and any other reason to a hard error, with usage and saw_stop recorded. */
static void test_codex_incomplete(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai_codex();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n"
         "data: {\"type\":\"response.incomplete\",\"response\":{\"id\":\"resp_inc\","
         "\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
         "\"usage\":{\"input_tokens\":9,\"output_tokens\":3}}}\n\n");
    agentc_sse_free(&sse);
    check("codex_incomplete_finish", p->finish(&st) == 0);
    stop(&st);
    check("codex_incomplete_stop", st.stop_reason == AGENTC_STOP_LENGTH && st.saw_stop);
    check("codex_incomplete_usage", st.usage_input == 9 && st.usage_output == 3);
    check("codex_incomplete_body", block_is(m, 0, AGENTC_BLK_TEXT, "partial"));
    check("codex_incomplete_id", agentc_streq(st.response_id, "resp_inc"));
    agentc_transcript_free(&tr);

    /* a non-length reason (content_filter) is reported as a provider error */
    start(&tr, &m, &st, &c, p);
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"type\":\"response.incomplete\",\"response\":{"
         "\"incomplete_details\":{\"reason\":\"content_filter\"}}}\n\n");
    agentc_sse_free(&sse);
    check("codex_incomplete_error", p->finish(&st) == -1 &&
                             st.stop_reason == AGENTC_STOP_ERROR &&
                             agentc_str_str(st.error, "content_filter") != NULL);
    stop(&st);
    agentc_transcript_free(&tr);
}

static void test_codex_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai_codex();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"plan \"}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":"
         "\"function_call\",\"call_id\":\"call_1\",\"name\":\"read\","
         "\"arguments\":\"\"}}\n\n"
         "data: {\"type\":\"response.function_call_arguments.delta\","
         "\"delta\":\"{\\\"path\\\":\"}\n\n"
         "data: {\"type\":\"response.function_call_arguments.delta\","
         "\"delta\":\"\\\"/tmp/a\\\"}\"}\n\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_2\","
         "\"usage\":{\"input_tokens\":7,\"output_tokens\":2}}}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("codex_tools_blocks", m->nblocks == 2);
    check("codex_tools_think", block_is(m, 0, AGENTC_BLK_THINK, "plan "));
    bool ok = m->nblocks == 2 && m->blocks[1].type == AGENTC_BLK_TOOLCALL &&
              agentc_streq(m->blocks[1].tool_id, "call_1") &&
              agentc_streq(m->blocks[1].tool_name, "read") &&
              agentc_streq(m->blocks[1].tool_args, "{\"path\":\"/tmp/a\"}");
    check("codex_tools_call", ok);
    check("codex_tools_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);

    /* an error event is surfaced as a provider error */
    start(&tr, &m, &st, &c, p);
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"message\":"
         "\"boom\"}}}\n\n");
    agentc_sse_free(&sse);
    check("codex_error", p->finish(&st) == -1 && st.stop_reason == AGENTC_STOP_ERROR &&
                             agentc_str_str(st.error, "boom") != NULL);
    stop(&st);
    agentc_transcript_free(&tr);
}

/* Parallel function_call items: output_item.added records the item id and
 * output_index; argument deltas (interleaved, some keyed only by
 * output_index) must route to the right block. */
static void test_codex_parallel_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai_codex();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":"
         "{\"type\":\"function_call\",\"id\":\"fc_a\",\"call_id\":\"call_a\","
         "\"name\":\"read\",\"arguments\":\"\"}}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":"
         "{\"type\":\"function_call\",\"id\":\"fc_b\",\"call_id\":\"call_b\","
         "\"name\":\"write\",\"arguments\":\"\"}}\n\n"
         "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_b\","
         "\"output_index\":1,\"delta\":\"{\\\"path\\\":\\\"y\\\",\"}\n\n"
         "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_a\","
         "\"output_index\":0,\"delta\":\"{\\\"path\\\":\\\"tmp/a\\\"}\"}\n\n"
         "data: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":1,"
         "\"delta\":\"\\\"content\\\":\\\"z\\\"}\"}\n\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_p\","
         "\"usage\":{\"input_tokens\":5,\"output_tokens\":3}}}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("codex_parallel_blocks", m->nblocks == 2);
    bool a = m->nblocks == 2 && m->blocks[0].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[0].tool_id, "call_a") &&
             agentc_streq(m->blocks[0].tool_name, "read") &&
             agentc_streq(m->blocks[0].tool_args, "{\"path\":\"tmp/a\"}");
    bool b = m->nblocks == 2 && m->blocks[1].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[1].tool_id, "call_b") &&
             agentc_streq(m->blocks[1].tool_name, "write") &&
             agentc_streq(m->blocks[1].tool_args, "{\"path\":\"y\",\"content\":\"z\"}");
    check("codex_parallel_call_a", a);
    check("codex_parallel_call_b", b);
    check("codex_parallel_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

/* A Responses argument delta that cannot be matched to a tracked function_call
 * (here the 33rd call, past CODEX_MAX_CALLS=32) must be dropped, not appended
 * to the last legitimate call's block. */
static void test_codex_tool_cap(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai_codex();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);

    AgcBuf evs = { 0 };
    for (int i = 0; i < 32; i++)
        agentc_buf_printf(&evs,
                          "data: {\"type\":\"response.output_item.added\",\"output_index\":%d,"
                          "\"item\":{\"type\":\"function_call\",\"id\":\"fc_%d\",\"call_id\":"
                          "\"call_%d\",\"name\":\"read\",\"arguments\":\"\"}}\n\n",
                          i, i, i);
    agentc_buf_cstr(&evs,
                    /* the 33rd function_call is past CODEX_MAX_CALLS: no block is
                     * created, so a later delta for it has nowhere to land */
                    "data: {\"type\":\"response.output_item.added\",\"output_index\":32,"
                    "\"item\":{\"type\":\"function_call\",\"id\":\"fc_32\",\"call_id\":"
                    "\"call_32\",\"name\":\"read\",\"arguments\":\"\"}}\n\n"
                    "data: {\"type\":\"response.function_call_arguments.delta\","
                    "\"item_id\":\"fc_31\",\"output_index\":31,\"delta\":\"{\\\"legit\\\":1}\"}\n\n"
                    "data: {\"type\":\"response.function_call_arguments.delta\","
                    "\"item_id\":\"fc_32\",\"output_index\":32,\"delta\":\"{\\\"evil\\\":1}\"}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_cap\","
                    "\"usage\":{\"input_tokens\":5,\"output_tokens\":2}}}\n\n");
    feed(&sse, &c, (const char *)evs.p);
    agentc_buf_free(&evs);
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("codex_cap_blocks", m->nblocks == 32);
    bool ok = m->nblocks == 32 && m->blocks[31].type == AGENTC_BLK_TOOLCALL &&
              agentc_streq(m->blocks[31].tool_args, "{\"legit\":1}");
    check("codex_cap_no_corrupt", ok);
    agentc_transcript_free(&tr);
}

/* ------------------------------------------------------------- anthropic */

static void test_anthropic_text(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_anthropic();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "event: message_start\n"
         "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01\",\"usage\":{"
         "\"input_tokens\":12,\"output_tokens\":0,\"cache_read_input_tokens\":3,"
         "\"cache_creation_input_tokens\":4}}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
         "\"text\",\"text\":\"\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"text_delta\",\"text\":\"Hello\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"text_delta\",\"text\":\" world\"}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
         "event: message_delta\n"
         "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\","
         "\"stop_sequence\":null},\"usage\":{\"output_tokens\":7}}\n\n"
         "event: message_stop\n"
         "data: {\"type\":\"message_stop\"}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("anthropic_text_blocks", m->nblocks == 1);
    check("anthropic_text_body", block_is(m, 0, AGENTC_BLK_TEXT, "Hello world"));
    check("anthropic_text_usage",
          st.usage_input == 12 && st.usage_output == 7 && st.usage_cache_read == 3 &&
              st.usage_cache_write == 4);
    check("anthropic_text_stop", st.stop_reason == AGENTC_STOP_STOP);
    check("anthropic_text_id", agentc_streq(st.response_id, "msg_01"));
    agentc_transcript_free(&tr);
}

static void test_anthropic_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_anthropic();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "event: message_start\n"
         "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_02\",\"usage\":{"
         "\"input_tokens\":20,\"output_tokens\":0}}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
         "\"thinking\",\"thinking\":\"\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"thinking_delta\",\"thinking\":\"Let me \"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"thinking_delta\",\"thinking\":\"check\"}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
         "\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\",\"input\":{}}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"/tmp/x\\\"\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\",\\\"limit\\\":5}\"}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
         "event: message_delta\n"
         "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
         "\"usage\":{\"output_tokens\":9}}\n\n"
         "event: message_stop\n"
         "data: {\"type\":\"message_stop\"}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("anthropic_tools_blocks", m->nblocks == 2);
    check("anthropic_tools_think", block_is(m, 0, AGENTC_BLK_THINK, "Let me check"));
    bool tool_ok = m->nblocks == 2 && m->blocks[1].type == AGENTC_BLK_TOOLCALL &&
                   agentc_streq(m->blocks[1].tool_id, "toolu_1") &&
                   agentc_streq(m->blocks[1].tool_name, "read") &&
                   agentc_streq(m->blocks[1].tool_args, "{\"path\":\"/tmp/x\",\"limit\":5}");
    check("anthropic_tools_call", tool_ok);
    check("anthropic_tools_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

/* Parallel tool_use blocks: two content_block_start events arrive before their
 * input_json_delta streams, so appending to the "last" tool block misattributes
 * arguments. Each delta is routed by its content-block `index`. */
static void test_anthropic_parallel_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_anthropic();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "event: message_start\n"
         "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_p\",\"usage\":{"
         "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
         "\"tool_use\",\"id\":\"toolu_a\",\"name\":\"read\",\"input\":{}}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
         "\"tool_use\",\"id\":\"toolu_b\",\"name\":\"write\",\"input\":{}}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"y\\\",\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"/tmp/a\\\"\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\"\\\"content\\\":\\\"z\\\"}\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\",\\\"limit\\\":3}\"}}\n\n"
         "event: message_delta\n"
         "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
         "\"usage\":{\"output_tokens\":6}}\n\n"
         "event: message_stop\n"
         "data: {\"type\":\"message_stop\"}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("anthropic_parallel_blocks", m->nblocks == 2);
    bool a = m->nblocks == 2 && m->blocks[0].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[0].tool_id, "toolu_a") &&
             agentc_streq(m->blocks[0].tool_name, "read") &&
             agentc_streq(m->blocks[0].tool_args, "{\"path\":\"/tmp/a\",\"limit\":3}");
    bool b = m->nblocks == 2 && m->blocks[1].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[1].tool_id, "toolu_b") &&
             agentc_streq(m->blocks[1].tool_name, "write") &&
             agentc_streq(m->blocks[1].tool_args, "{\"path\":\"y\",\"content\":\"z\"}");
    check("anthropic_parallel_call_a", a);
    check("anthropic_parallel_call_b", b);
    check("anthropic_parallel_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

/* Interleaved text/tool/text: each content block keeps its own position. Merging
 * every text delta into the last text block collapsed the trailing text into the
 * first block and dropped the middle ordering. */
static void test_anthropic_interleaved(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_anthropic();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "event: message_start\n"
         "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_i\",\"usage\":{"
         "\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
         "\"text\",\"text\":\"\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
         "\"text_delta\",\"text\":\"lead \"}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
         "\"tool_use\",\"id\":\"toolu_i\",\"name\":\"read\",\"input\":{}}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
         "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"/tmp/i\\\"}\"}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":"
         "\"text\",\"text\":\"\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
         "\"text_delta\",\"text\":\"trail\"}}\n\n"
         "event: message_delta\n"
         "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
         "\"usage\":{\"output_tokens\":5}}\n\n"
         "event: message_stop\n"
         "data: {\"type\":\"message_stop\"}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("anthropic_interleaved_blocks", m->nblocks == 3);
    check("anthropic_interleaved_b0", block_is(m, 0, AGENTC_BLK_TEXT, "lead "));
    bool call = m->nblocks == 3 && m->blocks[1].type == AGENTC_BLK_TOOLCALL &&
                agentc_streq(m->blocks[1].tool_name, "read") &&
                agentc_streq(m->blocks[1].tool_args, "{\"path\":\"/tmp/i\"}");
    check("anthropic_interleaved_call", call);
    check("anthropic_interleaved_b2", block_is(m, 2, AGENTC_BLK_TEXT, "trail"));
    check("anthropic_interleaved_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

static void test_anthropic_tool_cap(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_anthropic();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);

    AgcBuf evs = { 0 };
    for (int i = 0; i < 256; i++)
        agentc_buf_printf(&evs,
                          "event: content_block_start\n"
                          "data: {\"type\":\"content_block_start\",\"index\":%d,"
                          "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_%d\","
                          "\"name\":\"read\"}}\n\n",
                          i, i);
    agentc_buf_cstr(&evs,
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":255,\"delta\":"
                    "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"legit\\\":1}\"}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":256,\"content_block\":"
                    "{\"type\":\"tool_use\",\"id\":\"toolu_evil\",\"name\":\"read\"}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":256,\"delta\":"
                    "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"evil\\\":1}\"}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n");
    feed(&sse, &c, (const char *)evs.p);
    agentc_buf_free(&evs);
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("anthropic_cap_blocks", m->nblocks == 256);
    bool ok = m->nblocks == 256 && m->blocks[255].type == AGENTC_BLK_TOOLCALL &&
              agentc_streq(m->blocks[255].tool_args, "{\"legit\":1}");
    check("anthropic_cap_no_corrupt", ok);
    agentc_transcript_free(&tr);
}

static void test_anthropic_error(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_anthropic();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "event: error\n"
         "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":"
         "\"Overloaded\"}}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);
    check("anthropic_error_stop", st.stop_reason == AGENTC_STOP_ERROR);
    check("anthropic_error_text", agentc_str_str(st.error, "Overloaded") != NULL);
    agentc_transcript_free(&tr);
}

/* ---------------------------------------------------------------- openai */

static void test_openai_text(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
         "\"Hi \"},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
         "\"there\"},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{},"
         "\"finish_reason\":\"stop\"}]}\n\n"
         "data: {\"id\":\"chatcmpl-1\",\"choices\":[],\"usage\":{\"prompt_tokens\":9,"
         "\"completion_tokens\":2,\"prompt_tokens_details\":{\"cached_tokens\":1}}}\n\n"
         "data: [DONE]\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("openai_text_blocks", m->nblocks == 1);
    check("openai_text_body", block_is(m, 0, AGENTC_BLK_TEXT, "Hi there"));
    check("openai_text_usage",
          st.usage_input == 9 && st.usage_output == 2 && st.usage_cache_read == 1);
    check("openai_text_stop", st.stop_reason == AGENTC_STOP_STOP);
    check("openai_text_done", st.saw_stop);
    check("openai_text_id", agentc_streq(st.response_id, "chatcmpl-1"));
    agentc_transcript_free(&tr);
}

static void test_openai_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"id\":\"chatcmpl-2\",\"choices\":[{\"index\":0,\"delta\":{"
         "\"reasoning_content\":\"think \"},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-2\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
         "\"answer\"},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-2\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
         "[{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":"
         "\"read\",\"arguments\":\"{\\\"path\\\":\"}}]},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-2\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
         "[{\"index\":0,\"function\":{\"arguments\":\"\\\"/tmp/a\\\"}\"}}]},"
         "\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-2\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
         "[{\"index\":1,\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":"
         "\"write\",\"arguments\":\"{\\\"path\\\":\\\"x\\\",\"}}]},\"finish_reason\":"
         "null}]}\n\n"
         "data: {\"id\":\"chatcmpl-2\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
         "[{\"index\":1,\"function\":{\"arguments\":\"\\\"content\\\":\\\"y\\\"}\"}}]},"
         "\"finish_reason\":null}]}\n\n"
         "data: [DONE]\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("openai_tools_blocks", m->nblocks == 4);
    check("openai_tools_think", block_is(m, 0, AGENTC_BLK_THINK, "think "));
    check("openai_tools_text", block_is(m, 1, AGENTC_BLK_TEXT, "answer"));
    bool a = m->nblocks == 4 && m->blocks[2].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[2].tool_id, "call_a") &&
             agentc_streq(m->blocks[2].tool_name, "read") &&
             agentc_streq(m->blocks[2].tool_args, "{\"path\":\"/tmp/a\"}");
    bool b = m->nblocks == 4 && m->blocks[3].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[3].tool_id, "call_b") &&
             agentc_streq(m->blocks[3].tool_name, "write") &&
             agentc_streq(m->blocks[3].tool_args, "{\"path\":\"x\",\"content\":\"y\"}");
    check("openai_tools_call_a", a);
    check("openai_tools_call_b", b);
    check("openai_tools_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

/* Parallel tool calls: OpenAI keys each call by `index`. A batched opening
 * delta that carries several calls, followed by per-index argument deltas
 * (interleaved), must accumulate into distinct blocks. The old single-scalar
 * dedup appended a fresh empty-id block for a second call's continuation, so
 * the agent ran the wrong tool with `{}` arguments. */
static void test_openai_parallel_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"id\":\"chatcmpl-p\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
         "{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"read\","
         "\"arguments\":\"{\\\"path\\\":\"}},"
         "{\"index\":1,\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":\"write\","
         "\"arguments\":\"{\\\"path\\\":\\\"y\\\",\"}}]},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-p\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
         "{\"index\":1,\"function\":{\"arguments\":\"\\\"content\\\":\\\"z\\\"}\"}}]},"
         "\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-p\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
         "{\"index\":0,\"function\":{\"arguments\":\"\\\"tmp/a\\\"}\"}}]},"
         "\"finish_reason\":null}]}\n\n"
         "data: [DONE]\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("openai_parallel_blocks", m->nblocks == 2);
    bool a = m->nblocks == 2 && m->blocks[0].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[0].tool_id, "call_a") &&
             agentc_streq(m->blocks[0].tool_name, "read") &&
             agentc_streq(m->blocks[0].tool_args, "{\"path\":\"tmp/a\"}");
    bool b = m->nblocks == 2 && m->blocks[1].type == AGENTC_BLK_TOOLCALL &&
             agentc_streq(m->blocks[1].tool_id, "call_b") &&
             agentc_streq(m->blocks[1].tool_name, "write") &&
             agentc_streq(m->blocks[1].tool_args, "{\"path\":\"y\",\"content\":\"z\"}");
    check("openai_parallel_call_a", a);
    check("openai_parallel_call_b", b);
    check("openai_parallel_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

/* Out-of-range/negative tool_call indices: one call split across chunks must
 * not create a new empty block per delta. The fallback slot reuses a single
 * block while the same out-of-range index repeats. */
static void test_openai_out_of_range_index(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_openai();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"id\":\"chatcmpl-oor\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
         "{\"index\":500,\"id\":\"call_z\",\"type\":\"function\",\"function\":{\"name\":\"read\","
         "\"arguments\":\"{\\\"path\\\":\"}}]},\"finish_reason\":null}]}\n\n"
         "data: {\"id\":\"chatcmpl-oor\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
         "{\"index\":500,\"function\":{\"arguments\":\"\\\"/tmp/or\\\"}\"}}]},"
         "\"finish_reason\":null}]}\n\n"
         "data: [DONE]\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("openai_oor_blocks", m->nblocks == 1);
    bool ok = m->nblocks == 1 && m->blocks[0].type == AGENTC_BLK_TOOLCALL &&
              agentc_streq(m->blocks[0].tool_id, "call_z") &&
              agentc_streq(m->blocks[0].tool_name, "read") &&
              agentc_streq(m->blocks[0].tool_args, "{\"path\":\"/tmp/or\"}");
    check("openai_oor_call", ok);
    check("openai_oor_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

/* --------------------------------------------------------- build_request */

static void test_build_requests(void) {
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    AgcMsg *um = agentc_transcript_push(&tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(um, "hi", 2);

    AgcTool tools[1];
    agentc_tools_builtin(tools, 1);

    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    r.provider = "anthropic";
    r.model = "claude-sonnet-4-5";
    r.api_key = "sk-test";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 1;

    AgcBuf out = { 0 };
    int rc = agentc_prov_anthropic()->build_request(&out, &r, NULL, "/v1/messages");
    i64 k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    check("build_anthropic_ok", rc == 0 && k > 0);
    check("build_anthropic_head",
          agentc_str_starts((const char *)out.p, out.len, "content-type: application/json") &&
              agentc_str_str((const char *)out.p, "x-api-key: sk-test") != NULL);
    if (k > 0) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        check("build_anthropic_json",
              agentc_json_is(agentc_json_get(body, "model"), "claude-sonnet-4-5") &&
                  agentc_json_get_bool(body, "stream", false) &&
                  agentc_json_len(agentc_json_get(body, "messages")) == 1 &&
                  agentc_json_len(agentc_json_get(body, "tools")) == 1);
    } else {
        check("build_anthropic_json", false);
    }
    agentc_buf_free(&out);

    agentc_memset(&r, 0, sizeof r);
    r.provider = "openai";
    r.model = "gpt-5";
    r.api_key = "sk-test";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 1;
    out = (AgcBuf){ 0 };
    rc = agentc_prov_openai()->build_request(&out, &r, NULL, "/chat/completions");
    k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    check("build_openai_ok", rc == 0 && k > 0);
    check("build_openai_head",
          agentc_str_str((const char *)out.p, "authorization: Bearer sk-test") != NULL);
    if (k > 0) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        check("build_openai_json",
              agentc_json_is(agentc_json_get(body, "model"), "gpt-5") &&
                  agentc_json_get_bool(body, "stream", false) &&
                  agentc_json_get_bool(agentc_json_get(body, "stream_options"), "include_usage",
                                   false) &&
                  agentc_json_len(agentc_json_get(body, "messages")) == 2);
    } else {
        check("build_openai_json", false);
    }
    agentc_buf_free(&out);

    /* codex: Responses body + the account-id header, path /responses */
    agentc_memset(&r, 0, sizeof r);
    r.provider = "openai";
    r.model = "gpt-5-codex";
    r.api_key = "oat-1";
    r.account_id = "acct-1";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 1;
    r.max_tokens = 77;
    out = (AgcBuf){ 0 };
    rc = agentc_prov_openai_codex()->build_request(&out, &r, NULL, "/responses");
    k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    check("build_codex_ok", rc == 0 && k > 0);
    check("build_codex_head",
          agentc_str_str((const char *)out.p, "authorization: Bearer oat-1") != NULL &&
              agentc_str_str((const char *)out.p, "chatgpt-account-id: acct-1") != NULL &&
              agentc_str_str((const char *)out.p, "originator: agentc") != NULL);
    if (k > 0) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        AgcJson *input = agentc_json_get(body, "input");
        check("build_codex_json",
              agentc_json_is(agentc_json_get(body, "model"), "gpt-5-codex") &&
                  agentc_json_is(agentc_json_get(body, "instructions"), "sys") &&
                  agentc_json_get_bool(body, "store", true) == false &&
                  agentc_json_get_bool(body, "stream", false) &&
                  agentc_json_get_bool(body, "parallel_tool_calls", false) &&
                  agentc_json_len(input) == 1 &&
                  agentc_json_is(agentc_json_get(agentc_json_at(input, 0), "type"), "message") &&
                  agentc_json_len(agentc_json_get(body, "tools")) == 1);
        check("build_codex_max_output",
              agentc_json_get_int(body, "max_output_tokens", -1) == 77);
    } else {
        check("build_codex_json", false);
        check("build_codex_max_output", false);
    }
    agentc_buf_free(&out);
    agentc_transcript_free(&tr);
}

/* A credential or account id is untrusted input: a CR/LF in it must not forge
 * an extra request header line (openai and codex are the outliers the agent
 * normally sanitizes before this point). */
static void test_header_sanitized(void) {
    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    r.provider = "openai";
    r.model = "gpt-5";
    r.api_key = "sk\r\nX: forged";
    AgcBuf out = { 0 };
    agentc_prov_openai()->build_request(&out, &r, NULL, "/chat/completions");
    check("header_sanitized_openai",
          agentc_str_str((const char *)out.p, "\r\nX: forged") == NULL &&
              agentc_str_str((const char *)out.p, "authorization: Bearer skX: forged\r\n") !=
                  NULL);
    agentc_buf_free(&out);

    agentc_memset(&r, 0, sizeof r);
    r.provider = "openai";
    r.model = "gpt-5-codex";
    r.api_key = "oat-\r\nX: forged";
    r.account_id = "acct\r\nX: forged";
    out = (AgcBuf){ 0 };
    agentc_prov_openai_codex()->build_request(&out, &r, NULL, "/responses");
    check("header_sanitized_codex",
          agentc_str_str((const char *)out.p, "\r\nX: forged") == NULL &&
              agentc_str_str((const char *)out.p, "authorization: Bearer oat-X: forged\r\n") !=
                  NULL &&
              agentc_str_str((const char *)out.p, "chatgpt-account-id: acctX: forged\r\n") !=
                  NULL);
    agentc_buf_free(&out);
}

/* --------------------------------------------------- registry tests */

static int reg_build(AgcBuf *out, const AgcRequest *r, const char *url_host,
                    const char *url_path) {
    (void)r;
    (void)url_host;
    (void)url_path;
    agentc_buf_cstr(out, "content-type: application/json\r\n\r\n{\"reg\":1}");
    return 0;
}

static int reg_map(AgcStreamState *st, const AgcSseEvent *ev) {
    (void)ev;
    agentc_snprintf(st->response_id, sizeof st->response_id, "reg-map");
    return 0;
}

static int reg_finish(AgcStreamState *st) {
    (void)st;
    return 0;
}

static int reg_open(AgcStreamState *st) {
    st->priv = agentc_alloc(16);
    return 0;
}

static void reg_close(AgcStreamState *st) {
    agentc_free(st->priv);
    st->priv = NULL;
}

static AgcProviderOps reg_ops = {
    .name = "reg-additive",
    .api = "reg-additive-api",
    .path = "/reg",
    .default_base_url = "http://reg.test",
    .env_keys = { NULL, NULL, NULL },
    .needs_key = 0,
    .discover_style = AGENTC_DISCOVER_NONE,
    .build_request = reg_build,
    .map_sse = reg_map,
    .finish = reg_finish,
    .stream_open = reg_open,
    .stream_close = reg_close,
    .handle = NULL,
};

static AgcProviderOps reg_bad_ops = {
    .name = "reg-bad",
    .api = "",
};

static void test_provider_registry(void) {
    const AgcProvider *pa = agentc_prov_anthropic();
    const AgcProvider *po = agentc_prov_openai();
    const AgcProvider *pl = agentc_prov_ollama();
    const AgcProvider *pc = agentc_prov_ollama_cloud();
    const AgcProvider *px = agentc_prov_openai_codex();
    const AgcProviderOps *oa = agentc_provider_by_api("anthropic-messages");
    const AgcProviderOps *oo = agentc_provider_by_api("openai-chat");
    const AgcProviderOps *ox = agentc_provider_by_api("openai-codex-responses");
    const AgcProviderOps *ol = agentc_provider_by_name("ollama");
    const AgcProviderOps *oc = agentc_provider_by_name("ollama-cloud");
    bool ok = oa && oo && ox && ol && oc &&
              agentc_streq(oa->name, "anthropic") && agentc_streq(oa->path, "/v1/messages") &&
              agentc_streq(oo->path, "/chat/completions") &&
              agentc_streq(ox->name, "openai") && agentc_streq(ox->path, "/responses") &&
              agentc_provider_by_name("anthropic") == oa &&
              agentc_provider_by_name("openai") == oo &&
              agentc_provider_ops(pa) == oa && agentc_provider_ops(po) == oo &&
              agentc_provider_ops(pl) == ol && agentc_provider_ops(pc) == oc &&
              agentc_provider_ops(px) == ox && agentc_prov_anthropic() == pa &&
              agentc_prov_openai_codex() == px && agentc_prov_ollama() == pl;
    check("provider.registry.builtins", ok);
}

static void test_provider_additive(void) {
    int idx = agentc_provider_register(&reg_ops);
    const AgcProviderOps *by_api = agentc_provider_by_api("reg-additive-api");
    const AgcProviderOps *by_name = agentc_provider_by_name("reg-additive");
    const AgcProvider *h = agentc_provider_handle(&reg_ops);
    bool ok = idx >= 0 && by_api == &reg_ops && by_name == &reg_ops && h != NULL &&
              agentc_provider_ops(h) == &reg_ops && agentc_provider_handle(&reg_ops) == h;
    AgcBuf out = { 0 };
    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    ok = ok && h->build_request(&out, &r, NULL, "/reg") == 0 &&
         agentc_str_str((const char *)out.p, "{\"reg\":1}") != NULL;
    agentc_buf_free(&out);

    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    start(&tr, &m, &st, &c, h);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c, "data: {\"x\":1}\n\n");
    agentc_sse_free(&sse);
    ok = ok && agentc_streq(st.response_id, "reg-map");
    stop(&st);
    agentc_transcript_free(&tr);
    check("provider.registry.additive", ok);
}

static void test_provider_duplicate(void) {
    check("provider.registry.duplicate",
          agentc_provider_register(&reg_ops) == -17 &&
              agentc_provider_by_name("reg-additive") == &reg_ops);
}

static void test_provider_invalid(void) {
    check("provider.registry.invalid",
          agentc_provider_register(NULL) == -22 && agentc_provider_register(&reg_bad_ops) == -22 &&
              agentc_provider_by_api(NULL) == NULL && agentc_provider_by_name(NULL) == NULL);
}

/* Every adapter's stream_open/stream_close must balance exactly, including the
 * openai args accumulator that map_sse allocates. */
static void test_provider_priv_lifecycle(void) {
    const AgcProvider *provs[3] = { agentc_prov_anthropic(), agentc_prov_openai(),
                                       agentc_prov_openai_codex() };
    const char *events[3] = {
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
        "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"read\"}}\n\n",
        ("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"id\":\"c1\",\"function\":{\"name\":\"read\",\"arguments\":\"{\"}}]}}]}\n\n"),
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}\n\n",
    };
    size_t base = agentc_mem_live();
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        AgcTranscript tr;
        AgcMsg *m;
        AgcStreamState st;
        MapCtx c;
        start(&tr, &m, &st, &c, provs[i]);
        AgcSse sse;
        agentc_sse_init(&sse);
        feed(&sse, &c, events[i]);
        agentc_sse_free(&sse);
        ok = ok && agentc_mem_live() > base;
        stop(&st);
        agentc_transcript_free(&tr);
        ok = ok && agentc_mem_live() == base;
    }
    check("provider.priv.lifecycle", ok);
}

static void test_provider_dynamic(void) {
    const AgcProvider *a1 = agentc_prov_openai_compatible("reg-dyn-a", "http://reg-a/v1");
    const AgcProvider *a2 = agentc_prov_openai_compatible("reg-dyn-a", "http://ignored/v1");
    const AgcProvider *b1 = agentc_prov_openai_compatible("reg-dyn-b", "http://reg-b/v1");
    const AgcProvider *b2 = agentc_prov_openai_compatible("reg-dyn-b", "http://reg-b/v1");
    bool ok = a1 && a2 && b1 && b2 && a1 == a2 && b1 == b2 && a1 != b1 &&
              agentc_streq(a1->name, "reg-dyn-a") &&
              agentc_streq(a1->default_base_url, "http://reg-a/v1") &&
              agentc_streq(b1->default_base_url, "http://reg-b/v1") &&
              agentc_streq(a1->api, "openai-chat") &&
              agentc_provider_by_name("reg-dyn-a") == agentc_provider_ops(a1);
    check("provider.dynamic.stable", ok);
}

/* ------------------------------------------- preset registry tests */

static void test_provider_presets(void) {
    static const struct {
        const char *name;
        const char *base;
        const char *env0;
        const char *env1;
    } presets[] = {
        { "openrouter", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", NULL },
        { "xai", "https://api.x.ai/v1", "XAI_API_KEY", NULL },
        { "deepseek", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY", NULL },
        { "groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY", NULL },
        { "mistral", "https://api.mistral.ai/v1", "MISTRAL_API_KEY", NULL },
        { "together", "https://api.together.ai/v1", "TOGETHER_API_KEY", NULL },
        { "gemini", "https://generativelanguage.googleapis.com/v1beta/openai",
          "GEMINI_API_KEY", "GOOGLE_API_KEY" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++) {
        const AgcProviderOps *ops = agentc_provider_by_name(presets[i].name);
        const AgcProviderOps *again = agentc_provider_by_name(presets[i].name);
        ok = ok && ops && again == ops && agentc_streq(ops->api, "openai-chat") &&
             agentc_streq(ops->default_base_url, presets[i].base) &&
             agentc_streq(ops->env_keys[0], presets[i].env0) &&
             (presets[i].env1 ? agentc_streq(ops->env_keys[1], presets[i].env1)
                              : ops->env_keys[1] == NULL) &&
             ops->needs_key == 1 && ops->discover_style == AGENTC_DISCOVER_DEFAULT &&
             agentc_provider_handle((AgcProviderOps *)ops) != NULL;
    }
    check("provider.registry.presets", ok);
}

static void test_provider_preset_dynamic(void) {
    const AgcProviderOps *row = agentc_provider_by_name("xai");
    const AgcProvider *preset = row ? agentc_provider_handle((AgcProviderOps *)row) : NULL;
    const AgcProvider *compat = agentc_prov_openai_compatible("xai", "http://ignored/v1");
    bool ok = row && preset && compat == preset &&
              agentc_prov_openai_compatible("xai", "http://other/v1") == preset &&
              agentc_streq(preset->default_base_url, "https://api.x.ai/v1") &&
              agentc_provider_by_name("xai") == row;
    check("provider.registry.preset-vs-dynamic", ok);
}

/* A config-declared user gateway (providers.<id>.base_url) is reachable by name
 * even though the registry does not know the id; an unknown id with no base URL
 * stays unknown. */
static void test_provider_custom_gateway(void) {
    AgcConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);
    AgcConfigEntry prov = { .id = "mygw", .value = "http://localhost:8080/v1" };
    AgcConfigEntry key = { .id = "mygw", .value = "gw-key" };
    cfg.providers = &prov;
    cfg.nproviders = 1;
    cfg.api_keys = &key;
    cfg.napi_keys = 1;

    const AgcProvider *gw = agentc_setup_provider_for(&cfg, "mygw", NULL);
    check("custom_gateway_row",
          gw != NULL && agentc_streq(gw->api, "openai-chat") &&
              agentc_streq(gw->default_base_url, "http://localhost:8080/v1"));
    /* a user gateway may accept unauthenticated requests, like local Ollama */
    check("custom_gateway_no_key_required", !agentc_setup_needs_key("mygw"));

    agentc_setup_set_context(&cfg, NULL, NULL);
    check("custom_gateway_by_name", agentc_setup_provider("mygw") == gw);
    check("custom_gateway_unknown", agentc_setup_provider("nogw") == NULL &&
                                        agentc_setup_provider_for(&cfg, "nogw", NULL) == NULL);
    check("custom_gateway_key",
          agentc_streq(agentc_setup_resolve_key(&cfg, "mygw", NULL), "gw-key"));

    agentc_setup_set_context(NULL, NULL, NULL);
}

/* ----------------------------------------------- cost engine and cache math */
/* Anthropic input excludes cache read/creation (write 1.25x, read 0.1x).
 * OpenAI prompt_tokens INCLUDES cached tokens, so billing the full input plus
 * the cache read would double-bill; only the fresh input is charged at in_rate. */
static void test_model_cost(void) {
    AgcUsage u;
    agentc_memset(&u, 0, sizeof u);

    const AgcModel *sonnet = agentc_model_find("anthropic", "claude-sonnet-4-5");
    u.input = 1000;
    u.output = 500;
    u.cache_read = 2000;
    u.cache_write = 1000;
    check("cost_anthropic_cache", sonnet && agentc_model_cost(sonnet, &u) == 14850);
    check("cost_anthropic_rates",
          sonnet && sonnet->in_rate == 3000000 && sonnet->out_rate == 15000000 &&
              sonnet->cache_read_rate == 300000 && sonnet->cache_write_rate == 3750000 &&
              !(sonnet->rate_flags & AGENTC_MODEL_INPUT_INCLUDES_CACHE));

    const AgcModel *gpt5 = agentc_model_find("openai", "gpt-5");
    agentc_memset(&u, 0, sizeof u);
    u.input = 1000;
    u.output = 500;
    u.cache_read = 200;
    check("cost_openai_cache", gpt5 && agentc_model_cost(gpt5, &u) == 6125);
    /* double-billing the cached tokens would be 6375 */
    check("cost_openai_no_double_bill", gpt5 && agentc_model_cost(gpt5, &u) != 6375);

    const AgcModel *codex = agentc_model_find("openai", "gpt-5-codex");
    agentc_memset(&u, 0, sizeof u);
    u.input = 1000;
    u.output = 100;
    check("cost_unknown_model",
          codex && agentc_model_cost(codex, &u) == -1 &&
              !(codex->rate_flags & AGENTC_MODEL_RATE_KNOWN));
    check("cost_unknown_null", agentc_model_cost(NULL, &u) == -1);
}

/* ---------------------------------------------------------------- google */

static void test_google_text(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_google();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"pondering\","
         "\"thought\":true}],\"role\":\"model\"},\"index\":0}],"
         "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":0}}\n\n"
         "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hi \"}],"
         "\"role\":\"model\"},\"index\":0}]}\n\n"
         "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"there\"}],"
         "\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}],"
         "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":5,"
         "\"thoughtsTokenCount\":3,\"cachedContentTokenCount\":2},"
         "\"modelVersion\":\"gemini-2.5-flash\"}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("google_text_blocks", m->nblocks == 2);
    check("google_text_think", block_is(m, 0, AGENTC_BLK_THINK, "pondering"));
    check("google_text_body", block_is(m, 1, AGENTC_BLK_TEXT, "Hi there"));
    check("google_text_usage",
          st.usage_input == 10 && st.usage_output == 5 && st.usage_reasoning == 3 &&
              st.usage_cache_read == 2);
    check("google_text_stop", st.stop_reason == AGENTC_STOP_STOP && st.saw_stop);
    check("google_text_id", agentc_streq(st.response_id, "gemini-2.5-flash"));
    agentc_transcript_free(&tr);
}

static void test_google_tools(void) {
    AgcTranscript tr;
    AgcMsg *m;
    AgcStreamState st;
    MapCtx c;
    const AgcProvider *p = agentc_prov_google();
    start(&tr, &m, &st, &c, p);
    AgcSse sse;
    agentc_sse_init(&sse);
    feed(&sse, &c,
         "data: {\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{"
         "\"name\":\"read\",\"args\":{\"path\":\"/tmp/x\",\"limit\":5}}}],"
         "\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}]}\n\n");
    agentc_sse_free(&sse);
    p->finish(&st);
    stop(&st);

    check("google_tools_blocks", m->nblocks == 1);
    bool tool_ok = m->nblocks == 1 && m->blocks[0].type == AGENTC_BLK_TOOLCALL &&
                   agentc_streq(m->blocks[0].tool_name, "read") &&
                   agentc_streq(m->blocks[0].tool_args, "{\"path\":\"/tmp/x\",\"limit\":5}");
    check("google_tools_call", tool_ok);
    check("google_tools_stop", st.stop_reason == AGENTC_STOP_TOOLUSE);
    agentc_transcript_free(&tr);
}

static void test_google_request(void) {
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    AgcMsg *um = agentc_transcript_push(&tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(um, "hi", 2);
    AgcMsg *am = agentc_transcript_push(&tr, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_text(am, "a", 1);
    agentc_msg_add_tool_call(am, "call_0", "read");
    agentc_msg_tool_args_append(am, "{\"path\":\"x\"}", 14);
    AgcMsg *tm = agentc_transcript_push(&tr, AGENTC_ROLE_TOOL);
    agentc_msg_add_tool_result(tm, "call_0", "read", "ok");

    AgcTool tools[1];
    agentc_tools_builtin(tools, 1);

    AgcRequest r;
    agentc_memset(&r, 0, sizeof r);
    r.provider = "google";
    r.model = "gemini-2.5-flash";
    r.api_key = "sk-test";
    r.system = "sys";
    r.transcript = &tr;
    r.tools = tools;
    r.ntools = 1;
    r.thinking_level = 3;
    r.max_tokens = 20000;

    AgcBuf out = { 0 };
    int rc = agentc_prov_google()->build_request(&out, &r, NULL, "/models/x:streamGenerateContent");
    i64 k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    check("build_google_ok", rc == 0 && k > 0);
    check("build_google_head",
          agentc_str_str((const char *)out.p, "x-goog-api-key: sk-test") != NULL);
    if (k > 0) {
        AgcJson *body = agentc_json_parse((const char *)out.p + k + 4, out.len - (size_t)k - 4);
        AgcJson *si = agentc_json_get(body, "systemInstruction");
        AgcJson *siparts = agentc_json_get(si, "parts");
        AgcJson *contents = agentc_json_get(body, "contents");
        AgcJson *model_turn = agentc_json_at(contents, 1);
        AgcJson *mparts = agentc_json_get(model_turn, "parts");
        AgcJson *call = agentc_json_get(agentc_json_at(mparts, 1), "functionCall");
        AgcJson *tool_turn = agentc_json_at(contents, 2);
        AgcJson *tparts = agentc_json_get(tool_turn, "parts");
        AgcJson *resp = agentc_json_get(agentc_json_at(tparts, 0), "functionResponse");
        const AgcJson *respv = agentc_json_get(resp, "response");
        AgcJson *toolsv = agentc_json_get(body, "tools");
        AgcJson *decls = agentc_json_get(agentc_json_at(toolsv, 0), "functionDeclarations");
        AgcJson *gen = agentc_json_get(body, "generationConfig");
        AgcJson *think = agentc_json_get(gen, "thinkingConfig");
        bool ok = agentc_json_is(agentc_json_get(agentc_json_at(siparts, 0), "text"), "sys") &&
                  agentc_json_len(contents) == 3 &&
                  agentc_json_is(agentc_json_get(agentc_json_at(contents, 0), "role"), "user") &&
                  agentc_json_is(agentc_json_get(model_turn, "role"), "model") &&
                  agentc_json_is(agentc_json_get(call, "name"), "read") &&
                  agentc_json_is(agentc_json_get(agentc_json_get(call, "args"), "path"), "x") &&
                  agentc_json_is(agentc_json_get(tool_turn, "role"), "user") &&
                  agentc_json_is(agentc_json_get(resp, "name"), "read") &&
                  agentc_json_is(agentc_json_get(respv, "result"), "ok") &&
                  agentc_json_len(decls) == 1 &&
                  agentc_json_is(agentc_json_get(agentc_json_at(decls, 0), "name"), "read") &&
                  agentc_json_get_int(gen, "maxOutputTokens", 0) == 20000 &&
                  agentc_json_get_int(think, "thinkingBudget", 0) == 8192 &&
                  agentc_json_get_bool(think, "includeThoughts", false);
        check("build_google_json", ok);
    } else {
        check("build_google_json", false);
    }
    agentc_buf_free(&out);
    agentc_transcript_free(&tr);
}

static void test_retry_after(void) {
    check("ra_seconds", agentc_transport_parse_retry_after(" 120 ") == 120000);
    check("ra_zero", agentc_transport_parse_retry_after("0") == 0);
    check("ra_cap", agentc_transport_parse_retry_after("99999") == 3600000);
    check("ra_negative", agentc_transport_parse_retry_after("-5") == 0);
    check("ra_bad", agentc_transport_parse_retry_after("soon") == 0);
    /* IMF-fixdate; a past date yields 0, a far-future date clamps to 1 h */
    check("ra_date_past",
          agentc_transport_parse_retry_after("Wed, 21 Oct 2015 07:28:00 GMT") == 0);
    check("ra_date_future",
          agentc_transport_parse_retry_after("Fri, 31 Dec 9999 23:59:59 GMT") == 3600000);
    check("ra_date_bad_month",
          agentc_transport_parse_retry_after("Wed, 21 Xxx 2026 07:28:00 GMT") == 0);
    check("ra_date_bad_time",
          agentc_transport_parse_retry_after("Wed, 21 Oct 2026 99:28:00 GMT") == 0);
    /* absurd digit runs must not overflow the scanner (remote-controlled header) */
    check("ra_overflow_seconds",
          agentc_transport_parse_retry_after("99999999999999999999") == 0);
    check("ra_overflow_day",
          agentc_transport_parse_retry_after("Wed, 99999999999 Oct 2026 07:28:00 GMT") == 0);
    check("ra_overflow_year",
          agentc_transport_parse_retry_after("Wed, 21 Oct 99999999999 07:28:00 GMT") == 0);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_anthropic_text();
    test_anthropic_tools();
    test_anthropic_parallel_tools();
    test_anthropic_interleaved();
    test_anthropic_tool_cap();
    test_anthropic_error();
    test_openai_text();
    test_openai_tools();
    test_openai_parallel_tools();
    test_openai_out_of_range_index();
    test_google_text();
    test_google_tools();
    test_google_request();
    test_codex_text();
    test_codex_incomplete();
    test_codex_tools();
    test_codex_parallel_tools();
    test_codex_tool_cap();
    test_build_requests();
    test_header_sanitized();
    test_empty_assistant_skipped();
    test_max_tokens_field();
    test_hidden_tools_skipped();
    test_ollama_cloud_wire();
    test_google_wire();
    test_http_error_excerpt();
    test_retry_after();
    test_provider_registry();
    test_provider_additive();
    test_provider_duplicate();
    test_provider_invalid();
    test_provider_priv_lifecycle();
    test_provider_dynamic();
    test_provider_presets();
    test_provider_preset_dynamic();
    test_provider_custom_gateway();
    test_model_cost();

    return fails;
}
