/* agentc-client.c — command-line client for the framed agentc protocol (P5 C1).
 *
 *   agentc-client --socket PATH [--token T] state
 *   agentc-client --socket PATH [--token T] call METHOD [JSON]
 *   agentc-client --socket PATH [--token T] prompt TEXT
 *   agentc-client --socket PATH [--token T] events SECONDS
 *   agentc-client --socket PATH [--token T] cancel ID
 *   agentc-client --socket PATH [--token T] ui METHOD PROMPT [VALUE]
 *   agentc-client --socket PATH [--token T] session [ID]
 *   agentc-client --socket PATH [--token T] new-session [ID]
 *   agentc-client --socket PATH [--token T] attach [ID]
 *
 * `--session ID` attaches to that session before running the command. The
 * `session` subcommand prints the tracked session id; `new-session` creates a
 * session (or selects ID) and `attach` binds this connection to ID (or the
 * current session).
 *
 * The tool prints the response `result` as compact JSON and each event as a
 * JSON object on stderr, mirroring the --mode json vocabulary. The `ui`
 * subcommand raises a server dialog (a `ui_prompt` request) and answers the
 * returned `ui_request` from one stdin line: `y`/`n` for confirm, the chosen
 * option for select, or the text for input/editor. VALUE is the comma-separated
 * option list for select, or the default for the other methods.
 */
#include "agentc_client.h"
#include "cbor_json.h"
#include "plat.h"

static void out_json(const AgcCbor *v) {
    AgcBuf b = {0};
    if (v && agentc_cbor_to_json(v, &b) == 0) {
        (void)os_write(1, b.p, b.len);
        (void)os_write(1, "\n", 1);
    } else {
        (void)os_write(1, "null\n", 5);
    }
    agentc_buf_free(&b);
}

static int on_event(void *ud, const AgcProtoMsg *ev) {
    (void)ud;
    if (ev->kind != AGENTC_PROTO_EVENT)
        return 0;
    AgcBuf b = {0};
    if (ev->event && agentc_cbor_to_json(ev->event, &b) == 0) {
        (void)os_write(2, b.p, b.len);
        (void)os_write(2, "\n", 1);
    }
    agentc_buf_free(&b);
    return 0;
}

/* ------------------------------------------------------------- ui dialog */

typedef struct {
    AgcClient *c;
    const char *deflt;
    const char *const *options;
    size_t noptions;
} UiCtx;

static void ui_print(const AgcProtoMsg *ev) {
    AgcBuf b = {0};
    agentc_buf_cstr(&b, "ui: ");
    if (ev->method)
        agentc_buf_push(&b, ev->method, ev->method_len);
    agentc_buf_cstr(&b, ": ");
    AgcCbor *prompt = agentc_cbor_get(ev->params, "prompt");
    if (prompt) {
        size_t n = 0;
        const u8 *p = agentc_cbor_text(prompt, &n);
        if (p)
            agentc_buf_push(&b, p, n);
    }
    AgcCbor *opts = agentc_cbor_get(ev->params, "options");
    if (opts) {
        agentc_buf_cstr(&b, " [");
        for (size_t i = 0; i < agentc_cbor_len(opts); i++) {
            size_t n = 0;
            const u8 *p = agentc_cbor_text(agentc_cbor_at(opts, i), &n);
            if (i)
                agentc_buf_byte(&b, '|');
            if (p)
                agentc_buf_push(&b, p, n);
        }
        agentc_buf_byte(&b, ']');
    }
    agentc_buf_byte(&b, '\n');
    (void)os_write(2, b.p, b.len);
    agentc_buf_free(&b);
}

static int on_ui(void *ud, const AgcProtoMsg *ev) {
    UiCtx *ctx = ud;
    if (!ev || ev->kind != AGENTC_PROTO_UI_REQUEST)
        return 0;
    ui_print(ev);
    char line[1024];
    int n = os_read(0, line, sizeof line - 1);
    if (n < 0)
        n = 0;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    line[n] = 0;
    const char *m = ev->method ? ev->method : "";
    AgcCborArena *a = agentc_cbor_arena_new(0);
    AgcCbor *val = NULL;
    bool ok = true;
    if (agentc_str_eq(m, ev->method_len, "confirm", 7)) {
        bool yes;
        if (n == 0)
            yes = ctx->deflt ? agentc_streq(ctx->deflt, "true") : false;
        else
            yes = (line[0] == 'y' || line[0] == 'Y' || line[0] == '1');
        val = agentc_cbor_new_bool(a, yes);
    } else if (agentc_str_eq(m, ev->method_len, "select", 6)) {
        const char *pick = n > 0 ? line : ctx->deflt;
        if (!pick) {
            ok = false;
        } else {
            /* accept either the option text or its 0-based index */
            bool numeric = false;
            i64 ix = agentc_parse_i64(pick, agentc_strlen(pick), &numeric);
            if (numeric && ix >= 0 && (size_t)ix < ctx->noptions)
                pick = ctx->options[ix];
            val = agentc_cbor_new_text(a, pick, agentc_strlen(pick));
        }
    } else {
        const char *text = n > 0 ? line : (ctx->deflt ? ctx->deflt : "");
        val = agentc_cbor_new_text(a, text, agentc_strlen(text));
    }
    (void)agentc_client_ui_response(ctx->c, agentc_cbor_uint(ev->id), ok, val);
    agentc_cbor_arena_free(a);
    return 0;
}

/* ------------------------------------------------------------- commands */

static int cmd_ui(AgcClient *c, const char *method, const char *prompt, const char *extra) {
    if (!method || !prompt)
        return 2;
    AgcCborArena *a = agentc_cbor_arena_new(0);
    AgcCbor *params;
    const char *def = NULL;
    const char *optv[AGENTC_PROTO_MAX_UI_OPTIONS];
    size_t nopt = 0;
    bool select = agentc_streq(method, "select");
    if (select) {
        /* `extra` is a comma-separated option list. */
        const char *p = extra;
        char buf[128];
        while (p && *p && nopt < AGENTC_PROTO_MAX_UI_OPTIONS) {
            size_t k = 0;
            while (*p && *p != ',' && k + 1 < sizeof buf)
                buf[k++] = *p++;
            buf[k] = 0;
            if (*p == ',')
                p++;
            if (k) {
                char *dup = agentc_strdup(buf);
                if (dup)
                    optv[nopt++] = dup;
            }
        }
        params = agentc_cbor_new_map(a, 3);
        agentc_cbor_map_set(params, 0, agentc_cbor_new_text(a, "method", 6),
                            agentc_cbor_new_text(a, method, agentc_strlen(method)));
        agentc_cbor_map_set(params, 1, agentc_cbor_new_text(a, "prompt", 6),
                            agentc_cbor_new_text(a, prompt, agentc_strlen(prompt)));
        AgcCbor *arr = agentc_cbor_new_array(a, nopt);
        for (size_t i = 0; i < nopt; i++)
            agentc_cbor_array_set(arr, i, agentc_cbor_new_text(a, optv[i], agentc_strlen(optv[i])));
        agentc_cbor_map_set(params, 2, agentc_cbor_new_text(a, "options", 7), arr);
    } else {
        def = extra;
        params = agentc_cbor_new_map(a, def ? 3 : 2);
        agentc_cbor_map_set(params, 0, agentc_cbor_new_text(a, "method", 6),
                            agentc_cbor_new_text(a, method, agentc_strlen(method)));
        agentc_cbor_map_set(params, 1, agentc_cbor_new_text(a, "prompt", 6),
                            agentc_cbor_new_text(a, prompt, agentc_strlen(prompt)));
        if (def)
            agentc_cbor_map_set(params, 2, agentc_cbor_new_text(a, "default", 7),
                                agentc_cbor_new_text(a, def, agentc_strlen(def)));
    }
    UiCtx ctx = {c, def, optv, nopt};
    AgcCbor *r = agentc_client_request(c, "ui_prompt", params, on_ui, &ctx);
    int rc = 0;
    if (r || agentc_client_last_ok(c))
        out_json(r);
    else {
        rc = 1;
        (void)os_write(2, agentc_client_last_error(c), agentc_strlen(agentc_client_last_error(c)));
    }
    for (size_t i = 0; i < nopt; i++)
        agentc_free((void *)optv[i]);
    agentc_cbor_arena_free(a);
    return rc;
}

int agentc_main(int argc, char **argv) {
    const char *sock = NULL, *tcp = NULL, *token = NULL, *cmd = NULL;
    const char *arg1 = NULL, *arg2 = NULL, *arg3 = NULL;
    const char *session_sel = NULL;
    for (int i = 1; i < argc; i++) {
        if (agentc_streq(argv[i], "--socket") || agentc_streq(argv[i], "-s")) {
            if (++i >= argc)
                return 2;
            sock = argv[i];
        } else if (agentc_streq(argv[i], "--tcp")) {
            if (++i >= argc)
                return 2;
            tcp = argv[i];
        } else if (agentc_streq(argv[i], "--token")) {
            if (++i >= argc)
                return 2;
            token = argv[i];
        } else if (agentc_streq(argv[i], "--session")) {
            if (++i >= argc)
                return 2;
            session_sel = argv[i];
        } else if (agentc_streq(argv[i], "--help") || agentc_streq(argv[i], "-h")) {
            static const char usage[] =
                "usage: agentc-client --socket PATH [--token T] "
                "state|call METHOD [JSON]|prompt TEXT|events SEC|"
                "cancel ID|ui METHOD PROMPT [VALUE]|reconnect|resync [LIMIT]|"
                "session [ID]|new-session [ID]|attach [ID]\n";
            (void)os_write(1, usage, sizeof usage - 1);
            return 0;
        } else if (!cmd) {
            cmd = argv[i];
        } else if (!arg1) {
            arg1 = argv[i];
        } else if (!arg2) {
            arg2 = argv[i];
        } else if (!arg3) {
            arg3 = argv[i];
        }
    }
    char path[600];
    if (tcp)
        agentc_snprintf(path, sizeof path, "tcp:%s", tcp);
    else if (sock)
        agentc_snprintf(path, sizeof path, "%s", sock);
    else {
        (void)os_write(2, "agentc-client: --socket or --tcp is required\n", 46);
        return 2;
    }
    if (!cmd)
        return 2;

    AgcClient *c = agentc_client_connect(path, token);
    if (!c) {
        (void)os_write(2, "agentc-client: connect/handshake failed\n", 40);
        return 1;
    }
    (void)os_write(2, agentc_client_server_id(c), agentc_strlen(agentc_client_server_id(c)));
    (void)os_write(2, "\n", 1);

    if (session_sel && agentc_client_attach(c, session_sel, NULL, 0) != 0) {
        (void)os_write(2, agentc_client_last_error(c), agentc_strlen(agentc_client_last_error(c)));
        agentc_client_close(c);
        return 1;
    }

    int rc = 0;
    if (agentc_streq(cmd, "state")) {
        AgcCbor *r = agentc_client_request(c, "get_state", NULL, on_event, NULL);
        if (r || agentc_client_last_ok(c))
            out_json(r);
        else {
            rc = 1;
            (void)os_write(2, agentc_client_last_error(c),
                           agentc_strlen(agentc_client_last_error(c)));
        }
    } else if (agentc_streq(cmd, "call")) {
        if (!arg1) {
            rc = 2;
        } else {
            AgcCborArena *a = agentc_cbor_arena_new(0);
            AgcCbor *params = NULL;
            if (arg2) {
                int err = 0;
                params = agentc_cbor_from_json(a, arg2, agentc_strlen(arg2), &err);
            }
            AgcCbor *r = agentc_client_request(c, arg1, params, on_event, NULL);
            if (r || agentc_client_last_ok(c))
                out_json(r);
            else {
                rc = 1;
                (void)os_write(2, agentc_client_last_error(c),
                               agentc_strlen(agentc_client_last_error(c)));
            }
            agentc_cbor_arena_free(a);
        }
    } else if (agentc_streq(cmd, "prompt")) {
        if (!arg1) {
            rc = 2;
        } else {
            AgcCborArena *a = agentc_cbor_arena_new(0);
            AgcCbor *params = agentc_cbor_new_map(a, 1);
            agentc_cbor_map_set(params, 0, agentc_cbor_new_text(a, "text", 4),
                                agentc_cbor_new_text(a, arg1, agentc_strlen(arg1)));
            AgcCbor *r = agentc_client_request(c, "prompt", params, on_event, NULL);
            if (r || agentc_client_last_ok(c))
                out_json(r);
            else {
                rc = 1;
                (void)os_write(2, agentc_client_last_error(c),
                               agentc_strlen(agentc_client_last_error(c)));
            }
            agentc_cbor_arena_free(a);
        }
    } else if (agentc_streq(cmd, "events")) {
        int secs = 1;
        if (arg1) {
            bool ok = false;
            secs = (int)agentc_parse_i64(arg1, agentc_strlen(arg1), &ok);
            if (!ok)
                secs = 1;
        }
        if (secs < 1)
            secs = 1;
        int r = agentc_client_pump(c, secs * 1000, on_event, NULL);
        if (r < 0)
            rc = 1;
    } else if (agentc_streq(cmd, "cancel")) {
        bool ok = false;
        i64 id = arg1 ? agentc_parse_i64(arg1, agentc_strlen(arg1), &ok) : 0;
        AgcCborArena *a = agentc_cbor_arena_new(0);
        AgcCbor *idv = agentc_cbor_new_uint(a, ok ? (u64)id : 0);
        rc = agentc_client_cancel(c, idv) == 0 ? 0 : 1;
        agentc_cbor_arena_free(a);
    } else if (agentc_streq(cmd, "ui")) {
        rc = cmd_ui(c, arg1, arg2, arg3);
    } else if (agentc_streq(cmd, "session")) {
        if (arg1) {
            if (agentc_client_attach(c, arg1, NULL, 0) != 0)
                rc = 1;
        }
        if (rc == 0) {
            AgcBuf out = {0};
            agentc_buf_cstr(&out, "{\"session_id\":\"");
            agentc_buf_cstr(&out, agentc_client_session_id(c));
            agentc_buf_cstr(&out, "\"}\n");
            (void)os_write(1, out.p, out.len);
            agentc_buf_free(&out);
        } else {
            (void)os_write(2, agentc_client_last_error(c),
                           agentc_strlen(agentc_client_last_error(c)));
        }
    } else if (agentc_streq(cmd, "new-session")) {
        if (agentc_client_new_session(c, arg1) != 0) {
            rc = 1;
            (void)os_write(2, agentc_client_last_error(c),
                           agentc_strlen(agentc_client_last_error(c)));
        } else {
            AgcBuf out = {0};
            agentc_buf_cstr(&out, "{\"session_id\":\"");
            agentc_buf_cstr(&out, agentc_client_session_id(c));
            agentc_buf_cstr(&out, "\"}\n");
            (void)os_write(1, out.p, out.len);
            agentc_buf_free(&out);
        }
    } else if (agentc_streq(cmd, "attach")) {
        if (agentc_client_attach(c, arg1, NULL, 0) != 0) {
            rc = 1;
            (void)os_write(2, agentc_client_last_error(c),
                           agentc_strlen(agentc_client_last_error(c)));
        } else {
            AgcBuf out = {0};
            agentc_buf_cstr(&out, "{\"session_id\":\"");
            agentc_buf_cstr(&out, agentc_client_session_id(c));
            agentc_buf_cstr(&out, "\"}\n");
            (void)os_write(1, out.p, out.len);
            agentc_buf_free(&out);
        }
    } else if (agentc_streq(cmd, "reconnect")) {
        /* C-D8: explicit reconnect, no auto-replay. */
        int rr = agentc_client_reconnect(c);
        if (rr != 0) {
            rc = 1;
            (void)os_write(2, agentc_client_error(c), agentc_strlen(agentc_client_error(c)));
        } else {
            AgcBuf out = {0};
            agentc_buf_cstr(&out, "{\"reconnected\":true,\"replayed\":false,\"server_id\":\"");
            agentc_buf_cstr(&out, agentc_client_server_id(c));
            agentc_buf_cstr(&out, "\",\"session_id\":\"");
            agentc_buf_cstr(&out, agentc_client_session_id(c));
            agentc_buf_cstr(&out, "\"}\n");
            (void)os_write(1, out.p, out.len);
            agentc_buf_free(&out);
        }
    } else if (agentc_streq(cmd, "resync")) {
        /* C-D8: resync the transcript with bounded `get_messages` paging.
         * Nothing is replayed automatically; the client asks from cursor 0. */
        bool ok = false;
        i64 lim = arg1 ? agentc_parse_i64(arg1, agentc_strlen(arg1), &ok) : 100;
        if (!ok || lim < 1)
            lim = 100;
        if (lim > 1000)
            lim = 1000;
        u64 cursor = 0, total = 0, pages = 0, msgs = 0;
        for (;;) {
            AgcCborArena *a = agentc_cbor_arena_new(0);
            AgcCbor *params = agentc_cbor_new_map(a, 2);
            agentc_cbor_map_set(params, 0, agentc_cbor_new_text(a, "cursor", 6),
                                agentc_cbor_new_uint(a, cursor));
            agentc_cbor_map_set(params, 1, agentc_cbor_new_text(a, "limit", 5),
                                agentc_cbor_new_uint(a, (u64)lim));
            AgcCbor *r = agentc_client_request(c, "get_messages", params, on_event, NULL);
            if (r) {
                AgcCbor *tv = agentc_cbor_get(r, "total");
                AgcCbor *mv = agentc_cbor_get(r, "messages");
                AgcCbor *nv = agentc_cbor_get(r, "next_cursor");
                total = tv && agentc_cbor_type(tv) == AGENTC_CBOR_UINT ? agentc_cbor_uint(tv) : 0;
                msgs += mv && agentc_cbor_type(mv) == AGENTC_CBOR_ARRAY ? agentc_cbor_len(mv) : 0;
                pages++;
                if (!nv || agentc_cbor_type(nv) != AGENTC_CBOR_UINT) {
                    agentc_cbor_arena_free(a);
                    break;
                }
                cursor = agentc_cbor_uint(nv);
            } else {
                rc = 1;
                (void)os_write(2, agentc_client_last_error(c),
                               agentc_strlen(agentc_client_last_error(c)));
                agentc_cbor_arena_free(a);
                break;
            }
            agentc_cbor_arena_free(a);
            if (pages > 100000)
                break; /* defensive bound */
        }
        if (rc == 0) {
            AgcBuf out = {0};
            agentc_buf_printf(&out, "{\"total\":%llu,\"messages\":%llu,\"pages\":%llu}\n",
                              (unsigned long long)total, (unsigned long long)msgs,
                              (unsigned long long)pages);
            (void)os_write(1, out.p, out.len);
            agentc_buf_free(&out);
        }
    } else {
        rc = 2;
    }
    agentc_client_close(c);
    return rc;
}
