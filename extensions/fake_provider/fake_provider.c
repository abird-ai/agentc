/* fake_provider.c — example typed custom provider.
 *
 * Registers provider `fake` from init(): Bearer auth, no model discovery, one
 * static model and a request body built from the host-materialised request
 * view. The core owns transport, retries, cancellation, header sanitization,
 * Content-Length and the credential; this file only describes the wire
 * dialect:
 *
 *   request   POST /chat, JSON body with the transcript/tool view, Bearer auth
 *   response  SSE where each `data:` payload is one JSON event:
 *               {"kind":"text","text":"..."}
 *               {"kind":"thinking","text":"..."}
 *               {"kind":"tool_call","id":"call-1","name":"read","args":"{...}"}
 *               {"kind":"usage","input":1,"output":2,"cache_read":0,
 *                "reasoning":0}
 *               {"kind":"stop","reason":"stop|length|tool_use|error|aborted"}
 *               {"kind":"error","message":"..."}
 *             `[DONE]` is accepted as an equivalent of {"kind":"stop"}.
 *
 * Build (the extension manifest drives this; see extensions/README.md):
 *   clang -std=c23 -ffreestanding ... -I include \
 *       -Dagentc_ext_init=agentc_ext_fake_provider_init \
 *       -DFAKE_PROVIDER_EMIT_FORBIDDEN=1 -c extensions/fake_provider/fake_provider.c
 *
 * FAKE_PROVIDER_EMIT_FORBIDDEN makes build_request also write `Host:` into the
 * head sink. The core sanitizer must drop it (and Content-Length /
 * Transfer-Encoding) before the request reaches the wire; tests/ext.sh drives
 * that through the `make ext-pipeline` harness. Without the define the example
 * emits only legitimate headers.
 */
#include "agentc_ext.h"

#define FAKE_NAME     "fake"
#define FAKE_VERSION  "0.1.0"
#define FAKE_BASE_URL "http://127.0.0.1:9/v1"   /* discard port: never dialed */

/* ------------------------------------------------------------ small helpers */
/* The extension ABI intentionally exposes no libc, so these three helpers are
 * the whole string toolkit this example needs. */

static bool s_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static size_t s_len(const char *s) {
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static void out_cstr(const AgcExtHost *host, void *out, const char *s) {
    if (s) host->out_write(out, s, s_len(s));
}

/* Render `v` as an unsigned decimal number, byte by byte. */
static void out_u64(const AgcExtHost *host, void *out, unsigned long long v) {
    char tmp[24];
    size_t n = 0;
    if (v == 0) {
        host->out_write(out, "0", 1);
        return;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) host->out_write(out, &tmp[--n], 1);
}

/* JSON-escape `s` through the host helper (which owns the returned copy for
 * the duration of the call; it is freed straight away). json_escape escapes
 * the content only, so the surrounding quotes are written here. */
static void out_json_str(const AgcExtHost *host, void *out, const char *s) {
    host->out_write(out, "\"", 1);
    char *escaped = host->json_escape(s ? s : "");
    out_cstr(host, out, escaped);
    host->free(escaped);
    host->out_write(out, "\"", 1);
}

/* ------------------------------------------------------------- declaration */

static const AgcExtProviderAuth fake_auth = {
    .struct_size = sizeof(AgcExtProviderAuth),
    .kind = AGENTC_EXT_AUTH_BEARER,
    .header = "Authorization",
    .prefix = "Bearer ",
};

static const AgcExtProviderModel fake_models[] = {
    { .struct_size = sizeof(AgcExtProviderModel),
      .id = "fake-model-1",
      .name = "Fake Model 1",
      .ctx_window = 8192,
      .max_tokens = 1024,
      .reasoning = 0,
      .image = 0 },
};

/* Per-stream extension state: allocated in stream_open, released in
 * stream_close. The counter is deliberately trivial; it exists to show the
 * st->ud ownership rule. */
typedef struct {
    unsigned events;
} FakeStream;

static int fake_stream_open(const AgcExtHost *host, const AgcExtProvider *self,
                            AgcExtStream *st) {
    (void)self;
    FakeStream *fs = host->alloc(sizeof *fs);
    if (!fs) return -12;                    /* ENOMEM */
    fs->events = 0;
    st->ud = fs;
    return 0;
}

static void fake_stream_close(const AgcExtHost *host, const AgcExtProvider *self,
                              AgcExtStream *st) {
    (void)self;
    host->free(st->ud);
    st->ud = NULL;
}

/* --------------------------------------------------------- request builder */

/* Write the head block into head_out and the JSON body into body_out with
 * host->out_write; the core inserts the blank line, sanitizes, adds
 * Content-Length and the auth line. Note the view is borrowed for this call
 * only (strings must be copied if they must outlive it). */
static int fake_build_request(const AgcExtHost *host, const AgcExtProvider *self,
                              const AgcExtRequestView *req, void *head_out, void *body_out) {
    (void)self;
    static const char head[] =
        "content-type: application/json\r\n"
        "x-fake-provider: 1\r\n";
    host->out_write(head_out, head, sizeof head - 1);
#ifdef FAKE_PROVIDER_EMIT_FORBIDDEN
    /* Forbidden: the core drops Host/Content-Length/Transfer-Encoding from an
     * extension head, so this line never reaches the wire. */
    static const char poison[] = "Host: attacker.example\r\n";
    host->out_write(head_out, poison, sizeof poison - 1);
#endif

    out_cstr(host, body_out, "{\"provider\":");
    out_json_str(host, body_out, req->provider);
    out_cstr(host, body_out, ",\"model\":");
    out_json_str(host, body_out, req->model);
    out_cstr(host, body_out, ",\"stream\":true,\"thinking\":");
    out_u64(host, body_out, (unsigned long long)req->thinking_level);

    out_cstr(host, body_out, ",\"messages\":[");
    for (size_t i = 0; i < req->nmessages; i++) {
        const AgcExtMessageView *m = &req->messages[i];
        if (i) host->out_write(body_out, ",", 1);
        out_cstr(host, body_out, "{\"role\":");
        out_u64(host, body_out, (unsigned long long)m->role);
        out_cstr(host, body_out, ",\"blocks\":[");
        for (size_t j = 0; j < m->nblocks; j++) {
            const AgcExtBlockView *b = &m->blocks[j];
            if (j) host->out_write(body_out, ",", 1);
            out_cstr(host, body_out, "{\"type\":");
            out_u64(host, body_out, (unsigned long long)b->type);
            if (b->type == AGENTC_EXT_BLK_TOOLCALL) {
                out_cstr(host, body_out, ",\"id\":");
                out_json_str(host, body_out, b->tool_id);
                out_cstr(host, body_out, ",\"name\":");
                out_json_str(host, body_out, b->tool_name);
                out_cstr(host, body_out, ",\"args\":");
                out_json_str(host, body_out, b->tool_args);
            } else if (b->text) {
                out_cstr(host, body_out, ",\"text\":");
                out_json_str(host, body_out, b->text);
            }
            host->out_write(body_out, "}", 1);
        }
        out_cstr(host, body_out, "]}");
    }
    out_cstr(host, body_out, "],\"tools\":[");
    for (size_t i = 0; i < req->ntools; i++) {
        if (i) host->out_write(body_out, ",", 1);
        out_json_str(host, body_out, req->tools[i].name);
    }
    out_cstr(host, body_out, "]}");
    return 0;
}

/* ---------------------------------------------------------- stream mapping */

static int fake_stop_reason(const char *name) {
    if (s_eq(name, "length")) return AGENTC_EXT_STOP_LENGTH;
    if (s_eq(name, "tool_use")) return AGENTC_EXT_STOP_TOOLUSE;
    if (s_eq(name, "error")) return AGENTC_EXT_STOP_ERROR;
    if (s_eq(name, "aborted")) return AGENTC_EXT_STOP_ABORTED;
    return AGENTC_EXT_STOP_STOP;
}

/* Map one parsed SSE event to the typed sink. The sink copies every payload
 * immediately, so nothing borrowed from the event may be retained. */
static int fake_stream_event(const AgcExtHost *host, const AgcExtProvider *self,
                             AgcExtStream *st, const AgcExtWireEvent *ev,
                             const AgcExtStreamSink *sink) {
    (void)self;
    FakeStream *fs = st->ud;
    if (fs) fs->events++;
    if (!ev || !ev->data) return 0;

    if (ev->data_len == 6 && s_eq(ev->data, "[DONE]")) {
        sink->stop(st, AGENTC_EXT_STOP_STOP);
        return 0;
    }

    const char *kind = host->json_get_str(ev->data, "kind", "");
    if (s_eq(kind, "text")) {
        const char *text = host->json_get_str(ev->data, "text", "");
        sink->text(st, text, s_len(text));
    } else if (s_eq(kind, "thinking")) {
        const char *text = host->json_get_str(ev->data, "text", "");
        sink->thinking(st, text, s_len(text));
    } else if (s_eq(kind, "tool_call")) {
        const char *id = host->json_get_str(ev->data, "id", "");
        const char *name = host->json_get_str(ev->data, "name", "");
        const char *args = host->json_get_str(ev->data, "args", "");
        sink->tool_start(st, id, name);
        if (args[0]) sink->tool_args(st, args, s_len(args));
    } else if (s_eq(kind, "usage")) {
        AgcExtUsage u;
        u.struct_size = sizeof u;
        u.fields = AGENTC_EXT_USAGE_INPUT | AGENTC_EXT_USAGE_OUTPUT |
                   AGENTC_EXT_USAGE_CACHE_READ | AGENTC_EXT_USAGE_CACHE_WRITE |
                   AGENTC_EXT_USAGE_REASONING;
        u.input = (uint32_t)host->json_get_int(ev->data, "input", 0);
        u.output = (uint32_t)host->json_get_int(ev->data, "output", 0);
        u.cache_read = (uint32_t)host->json_get_int(ev->data, "cache_read", 0);
        u.cache_write = (uint32_t)host->json_get_int(ev->data, "cache_write", 0);
        u.reasoning = (uint32_t)host->json_get_int(ev->data, "reasoning", 0);
        sink->usage(st, &u);
    } else if (s_eq(kind, "stop")) {
        sink->stop(st, fake_stop_reason(host->json_get_str(ev->data, "reason", "stop")));
    } else if (s_eq(kind, "error")) {
        const char *message =
            host->json_get_str(ev->data, "message", "fake provider error");
        sink->error(st, message);
    }
    return 0;
}

/* ---------------------------------------------------------- registration */

static const AgcExtProvider fake_provider = {
    .struct_size = sizeof(AgcExtProvider),
    .name = FAKE_NAME,
    .label = "Fake provider (example)",
    .default_base_url = FAKE_BASE_URL,
    .path = "/chat",
    .env_keys = { "FAKE_PROVIDER_API_KEY", NULL, NULL },
    .needs_key = 1,
    .discover_style = AGENTC_EXT_DISCOVER_NONE,
    .auth = &fake_auth,
    .models = fake_models,
    .nmodels = sizeof fake_models / sizeof fake_models[0],
    .ud = NULL,
    .build_request = fake_build_request,
    .stream_open = fake_stream_open,
    .stream_event = fake_stream_event,
    .stream_finish = NULL,                /* the core resolves the stop */
    .stream_close = fake_stream_close,
};

static int fake_init(const AgcExtHost *host) {
    /* add_provider was appended to the host vtable; an older host simply does
     * not offer it, so gate on the service rather than the ABI number. */
    if (!AGENTC_EXT_HOST_HAS(host, add_provider)) {
        host->log(3, "fake_provider: host cannot register providers");
        return 0;
    }
    host->add_provider(&fake_provider);
    return 0;
}

int agentc_ext_init(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "fake_provider";
    out->version = FAKE_VERSION;
    out->order = 0;
    out->init = fake_init;
    out->shutdown = NULL;
    out->required_host_size = 0;
    return 0;
}
