/* client.c — synchronous client for the framed agentc protocol (P5 Axis C).
 *
 * See include/agentc_client.h. Bounded, single-threaded; the decoder holds at
 * most one frame and the DOM arena one decoded value.
 */
#include "agentc_client.h"

#include "frame.h"
#include "net.h"
#include "plat.h"

#define CLIENT_BUF 65536

struct AgcClient {
    int fd;
    AgcFrameDecoder dec;
    AgcCborArena *arena;
    AgcProtoMsg msg;
    AgcCbor *root;
    bool got;
    char server_id[37];
    char session_id[64];
    bool has_cancel;
    bool has_ui;
    bool has_attach;
    bool has_multi_session;
    char *path;
    char *token;
    u64 next_id;
    bool last_ok;
    char last_code[32];
    char last_error[256];
    char error[256];
};

static void client_set_error(AgcClient *c, const char *msg) {
    if (!c)
        return;
    agentc_snprintf(c->error, sizeof c->error, "%s", msg ? msg : "error");
}

static void client_clear_last(AgcClient *c) {
    c->last_ok = false;
    c->last_code[0] = 0;
    c->last_error[0] = 0;
}

static u32 client_ip_word(const struct agentc_ip4 *ip) {
    u32 w;
    agentc_memcpy(&w, ip->b, 4);
    return w;
}

static int client_send_all(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = agentc_net_send(fd, q, n);
        if (w < 0)
            return w;
        if (w == 0)
            return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static int client_dial(const char *path) {
    if (agentc_str_starts(path, agentc_strlen(path), "tcp:")) {
        const char *spec = path + 4;
        const char *colon = NULL;
        for (const char *p = spec; *p; p++)
            if (*p == ':')
                colon = p;
        if (!colon)
            return -22;
        char host[128];
        size_t hn = (size_t)(colon - spec);
        if (hn == 0 || hn >= sizeof host)
            return -22;
        agentc_memcpy(host, spec, hn);
        host[hn] = 0;
        struct agentc_ip4 ip;
        if (!agentc_net_is_ip4(host, &ip))
            return -22;
        bool ok = false;
        i64 pv = agentc_parse_i64(colon + 1, agentc_strlen(colon + 1), &ok);
        if (!ok || pv < 0 || pv > 65535)
            return -22;
        int fd = agentc_net_socket();
        if (fd < 0)
            return fd;
        int r = agentc_net_connect(fd, client_ip_word(&ip), (u16)pv, 5000);
        if (r == -115) {
            int pr = agentc_net_poll(fd, OS_POLLOUT, 5000);
            if (pr <= 0) {
                agentc_net_close(fd);
                return pr < 0 ? pr : -110;
            }
            r = agentc_net_so_error(fd);
        }
        if (r < 0) {
            agentc_net_close(fd);
            return r;
        }
        return fd;
    }
    return agentc_net_unix_connect(path);
}

/* Decode one frame and stop the feed after it (the rest stays buffered). */
static int client_frame_cb(void *ud, const AgcFrame *f) {
    AgcClient *c = ud;
    int err = 0;
    AgcCbor *v = agentc_cbor_decode(c->arena, f->payload, f->len, NULL, &err);
    if (!v) {
        client_set_error(c, "malformed CBOR");
        return -71;
    }
    if (agentc_proto_decode(v, &c->msg) != 0) {
        client_set_error(c, "malformed envelope");
        return -71;
    }
    c->root = v;
    c->got = true;
    return 1;
}

/* Read the next envelope. -> 1 got, 0 timeout, negative errno / -1 eof. */
static int client_next(AgcClient *c, int timeout_ms) {
    c->got = false;
    int r = agentc_frame_decoder_feed(&c->dec, NULL, 0, client_frame_cb, c);
    if (r < 0)
        return r;
    if (c->got)
        return 1;
    i64 deadline = timeout_ms < 0 ? -1 : os_now_ns(OS_CLOCK_MONOTONIC) + (i64)timeout_ms * 1000000;
    for (;;) {
        int wait = -1;
        if (deadline >= 0) {
            i64 left = (deadline - os_now_ns(OS_CLOCK_MONOTONIC)) / 1000000;
            if (left < 0)
                left = 0;
            wait = (int)left;
        }
        int pr = agentc_net_poll(c->fd, OS_POLLIN, wait);
        if (pr < 0)
            return pr;
        if (pr == 0)
            return 0;
        u8 buf[CLIENT_BUF];
        int n = agentc_net_recv(c->fd, buf, sizeof buf);
        if (n == -11)
            continue;
        if (n < 0)
            return n;
        if (n == 0)
            return -1;
        r = agentc_frame_decoder_feed(&c->dec, buf, (size_t)n, client_frame_cb, c);
        if (r < 0)
            return r;
        if (c->got)
            return 1;
    }
}

static void client_copy_id(char *dst, size_t cap, const char *src) {
    if (!src) {
        dst[0] = 0;
        return;
    }
    agentc_snprintf(dst, cap, "%s", src);
}

static int client_handshake(AgcClient *c) {
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_hello(&w, AGENTC_PROTO_VERSION, "agentc-client/1", c->token);
    agentc_cborw_finish(&w);
    int r = client_send_all(c->fd, frame.p, frame.len);
    agentc_buf_free(&frame);
    if (r < 0)
        return r;
    r = client_next(c, 10000);
    if (r <= 0)
        return r ? r : -110;
    if (c->msg.kind == AGENTC_PROTO_HELLO_ACK) {
        client_copy_id(c->server_id, sizeof c->server_id, c->msg.server_id);
        client_copy_id(c->session_id, sizeof c->session_id, c->msg.session_id);
        c->has_cancel = false;
        c->has_ui = false;
        c->has_attach = false;
        c->has_multi_session = false;
        AgcCbor *feat = agentc_cbor_get(c->root, "features");
        for (size_t i = 0; i < agentc_cbor_len(feat); i++) {
            AgcCbor *f = agentc_cbor_at(feat, i);
            if (f && agentc_cbor_type(f) == AGENTC_CBOR_TEXT) {
                size_t n = 0;
                const u8 *p = agentc_cbor_text(f, &n);
                if (agentc_str_eq((const char *)p, n, "cancel", 6))
                    c->has_cancel = true;
                if (agentc_str_eq((const char *)p, n, "ui", 2))
                    c->has_ui = true;
                if (agentc_str_eq((const char *)p, n, "attach", 6))
                    c->has_attach = true;
                if (agentc_str_eq((const char *)p, n, "multi_session", 13))
                    c->has_multi_session = true;
            }
        }
        return 0;
    }
    if (c->msg.kind == AGENTC_PROTO_HELLO_ERROR) {
        client_set_error(c, c->msg.error_message ? c->msg.error_message : "hello rejected");
        return -13;
    }
    client_set_error(c, "unexpected handshake reply");
    return -71;
}

AgcClient *agentc_client_connect(const char *path, const char *token) {
    if (!path || !path[0])
        return NULL;
    AgcClient *c = agentc_alloc(sizeof *c);
    agentc_memset(c, 0, sizeof *c);
    c->arena = agentc_cbor_arena_new(0);
    c->path = agentc_strdup(path);
    c->token = token ? agentc_strdup(token) : NULL;
    c->fd = client_dial(path);
    if (c->fd < 0) {
        client_set_error(c, "cannot connect");
        agentc_client_close(c);
        return NULL;
    }
    agentc_frame_decoder_init(&c->dec);
    if (client_handshake(c) != 0) {
        agentc_client_close(c);
        return NULL;
    }
    return c;
}

void agentc_client_close(AgcClient *c) {
    if (!c)
        return;
    if (c->fd >= 0)
        agentc_net_close(c->fd);
    agentc_frame_decoder_free(&c->dec);
    agentc_cbor_arena_free(c->arena);
    agentc_free(c->path);
    agentc_free(c->token);
    agentc_free(c);
}

const char *agentc_client_server_id(const AgcClient *c) {
    return c ? c->server_id : "";
}
const char *agentc_client_session_id(const AgcClient *c) {
    return c ? c->session_id : "";
}
bool agentc_client_has_feature(const AgcClient *c, const char *name) {
    if (!c || !name)
        return false;
    if (agentc_streq(name, "cancel"))
        return c->has_cancel;
    if (agentc_streq(name, "ui"))
        return c->has_ui;
    if (agentc_streq(name, "attach"))
        return c->has_attach;
    if (agentc_streq(name, "multi_session"))
        return c->has_multi_session;
    return false;
}
const char *agentc_client_error(const AgcClient *c) {
    return c ? c->error : "no client";
}
bool agentc_client_last_ok(const AgcClient *c) {
    return c && c->last_ok;
}
const char *agentc_client_last_code(const AgcClient *c) {
    return c ? c->last_code : "";
}
const char *agentc_client_last_error(const AgcClient *c) {
    return c ? c->last_error : "";
}

AgcCbor *agentc_client_request(AgcClient *c, const char *method, const AgcCbor *params,
                               AgcClientEventFn on_event, void *ud) {
    if (!c || !method)
        return NULL;
    u64 id = ++c->next_id;
    AgcCbor *idv = agentc_cbor_new_uint(c->arena, id);
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_request(&w, idv, method, agentc_strlen(method), params);
    agentc_cborw_finish(&w);
    int r = client_send_all(c->fd, frame.p, frame.len);
    agentc_buf_free(&frame);
    if (r < 0) {
        client_set_error(c, "send failed");
        return NULL;
    }
    client_clear_last(c);
    for (;;) {
        r = client_next(c, -1);
        if (r <= 0) {
            client_set_error(c, r == 0 ? "timeout" : "connection closed");
            return NULL;
        }
        if (c->msg.kind == AGENTC_PROTO_RESPONSE) {
            if (agentc_cbor_type(c->msg.id) == AGENTC_CBOR_UINT &&
                agentc_cbor_uint(c->msg.id) == id) {
                if (c->msg.ok) {
                    c->last_ok = true;
                    return c->msg.result;
                }
                client_copy_id(c->last_code, sizeof c->last_code, c->msg.error_code);
                client_copy_id(c->last_error, sizeof c->last_error, c->msg.error_message);
                return NULL;
            }
        } else if (c->msg.kind == AGENTC_PROTO_EVENT || c->msg.kind == AGENTC_PROTO_UI_REQUEST) {
            if (on_event && on_event(ud, &c->msg))
                return NULL;
        } else if (c->msg.kind == AGENTC_PROTO_HELLO_ERROR) {
            client_copy_id(c->last_error, sizeof c->last_error, c->msg.error_message);
            return NULL;
        }
    }
}

int agentc_client_cancel(AgcClient *c, const AgcCbor *id) {
    if (!c || !id)
        return -22;
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_cancel(&w, id);
    agentc_cborw_finish(&w);
    int r = client_send_all(c->fd, frame.p, frame.len);
    agentc_buf_free(&frame);
    return r;
}

int agentc_client_ui_response(AgcClient *c, u64 id, bool ok, const AgcCbor *value) {
    if (!c)
        return -22;
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_ui_response(&w, id, ok, ok ? value : NULL);
    agentc_cborw_finish(&w);
    int r = client_send_all(c->fd, frame.p, frame.len);
    agentc_buf_free(&frame);
    return r;
}

int agentc_client_pump(AgcClient *c, int timeout_ms, AgcClientEventFn on_event, void *ud) {
    if (!c)
        return -22;
    for (;;) {
        int r = client_next(c, timeout_ms);
        if (r < 0)
            return r;
        if (r == 0)
            return 0;
        if (c->msg.kind == AGENTC_PROTO_EVENT || c->msg.kind == AGENTC_PROTO_UI_REQUEST) {
            if (on_event && on_event(ud, &c->msg))
                return 0;
        }
        timeout_ms = 0; /* drain buffered frames without another wait */
    }
}

int agentc_client_reconnect(AgcClient *c) {
    if (!c)
        return -22;
    if (c->fd >= 0)
        agentc_net_close(c->fd);
    agentc_frame_decoder_free(&c->dec);
    agentc_frame_decoder_init(&c->dec);
    agentc_cbor_arena_clear(c->arena);
    c->fd = client_dial(c->path);
    if (c->fd < 0) {
        client_set_error(c, "cannot reconnect");
        return c->fd;
    }
    return client_handshake(c);
}

/* Send one already-encoded envelope and wait for the response with `id`,
 * dropping any interleaved events. Returns the response result or NULL. */
static AgcCbor *client_wait_response(AgcClient *c, u64 id) {
    for (;;) {
        int r = client_next(c, -1);
        if (r <= 0) {
            if (r == 0)
                client_set_error(c, "timeout");
            else
                client_set_error(c, "connection closed");
            return NULL;
        }
        if (c->msg.kind == AGENTC_PROTO_RESPONSE &&
            agentc_cbor_type(c->msg.id) == AGENTC_CBOR_UINT && agentc_cbor_uint(c->msg.id) == id) {
            if (c->msg.ok)
                return c->msg.result;
            client_copy_id(c->last_code, sizeof c->last_code, c->msg.error_code);
            client_copy_id(c->last_error, sizeof c->last_error, c->msg.error_message);
            return NULL;
        }
    }
}

int agentc_client_new_session(AgcClient *c, const char *spec) {
    if (!c)
        return -22;
    AgcCborArena *a = agentc_cbor_arena_new(0);
    AgcCbor *params = NULL;
    if (spec && spec[0]) {
        params = agentc_cbor_new_map(a, 1);
        agentc_cbor_map_set(params, 0, agentc_cbor_new_text(a, "session", 7),
                            agentc_cbor_new_text(a, spec, agentc_strlen(spec)));
    }
    AgcCbor *r = agentc_client_request(c, "new_session", params, NULL, NULL);
    int rc = 0;
    if (r) {
        AgcCbor *sid = agentc_cbor_get(r, "session_id");
        if (sid && agentc_cbor_type(sid) == AGENTC_CBOR_TEXT)
            client_copy_id(c->session_id, sizeof c->session_id,
                           (const char *)agentc_cbor_text(sid, NULL));
    } else {
        rc = -1;
    }
    agentc_cbor_arena_free(a);
    return rc;
}

int agentc_client_attach(AgcClient *c, const char *session, const char *const *events,
                         size_t nevents) {
    if (!c)
        return -22;
    u64 id = ++c->next_id;
    AgcCbor *idv = agentc_cbor_new_uint(c->arena, id);
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_attach(&w, idv, session, events, nevents);
    agentc_cborw_finish(&w);
    int r = client_send_all(c->fd, frame.p, frame.len);
    agentc_buf_free(&frame);
    if (r < 0) {
        client_set_error(c, "send failed");
        return r;
    }
    client_clear_last(c);
    AgcCbor *res = client_wait_response(c, id);
    if (!res) {
        if (c->last_error[0] == 0)
            client_set_error(c, "attach failed");
        return -1;
    }
    if (session && session[0])
        client_copy_id(c->session_id, sizeof c->session_id, session);
    c->last_ok = true;
    return 0;
}

const char *agentc_client_event_session(const AgcProtoMsg *ev) {
    return (ev && ev->session_id) ? ev->session_id : "";
}
