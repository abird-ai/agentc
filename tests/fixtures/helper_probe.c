/* tests/fixtures/helper_probe.c — W3 helper-process fixture.
 *
 * A real, separate freestanding process that speaks the framed JSON
 * request/response protocol from include/frame.h. tests/ext.sh builds it with
 * the project's test flags and points ext_harness at it through
 * AGENTC_HELPER_FIXTURE; the harness registers it with
 * agentc_ext_register_helper() and exercises its proxy rows.
 *
 * Behavior (deterministic, so the harness can assert exact outcomes):
 *   - startup: one manifest frame
 *   - op "tool"    name "helper_echo": result is the JSON string payload.text
 *   - op "hook"    point "tool_call": {"block":true,...} when
 *                  payload.input.helper_block is true, else {"helper_alive":true}
 *   - op "hook"    point "agent_end": {"helper_observed":true} (observe)
 *   - op "command" name "helper_ping": result is the JSON string "pong"
 *   - op "shutdown": acknowledge, then exit
 *
 * Protocol bytes go to fd 1 directly (os_write); they are the wire, not user
 * terminal output, exactly like an MCP stdio server.
 */
#include "agentc.h"
#include "plat.h"
#include "wire.h"
#include "frame.h"

static const char g_manifest[] =
    "{\"name\":\"helper_probe\",\"version\":\"1.0.0\","
    "\"tools\":[{\"name\":\"helper_echo\","
    "\"desc\":\"Echo text from the helper process.\","
    "\"params_json\":\"{\\\"type\\\":\\\"object\\\",\\\"properties\\\":{"
    "\\\"text\\\":{\\\"type\\\":\\\"string\\\"}},\\\"required\\\":[\\\"text\\\"]}\","
    "\"flags\":1}],"
    "\"hooks\":[{\"point\":\"tool_call\",\"caps\":2,\"priority\":10},"
    "{\"point\":\"agent_end\",\"caps\":1,\"priority\":10}],"
    "\"commands\":[{\"name\":\"helper_ping\",\"desc\":\"Ping the helper process.\"}]}";

static bool g_shutdown;
static bool g_stall;

static int probe_write_all(int fd, const void *p, size_t n);

/* A-H1/A-L1 fixture: a manifest that advertises one tool which declares a
 * ~1 MiB frame and then dribbles one payload byte every 50 ms, never completing
 * it, and a SIGTERM handler that ignores the default action so the host must
 * escalate to SIGKILL. */
static const char g_stall_manifest[] =
    "{\"name\":\"helper_probe\",\"version\":\"1.0.0\","
    "\"tools\":[{\"name\":\"helper_stall\","
    "\"desc\":\"Stall a partial frame.\","
    "\"params_json\":\"{\\\"type\\\":\\\"object\\\"}\",\"flags\":1}]}";

static void sig_noop(int sig) {
    (void)sig; /* deliberately ignore SIGTERM: force the host to escalate */
}

static void probe_stall_frame(void) {
    u32 len = 1024u * 1024u;
    u8 hdr[AGENTC_FRAME_HEADER_BYTES] = {(u8)(len >> 24), (u8)(len >> 16), (u8)(len >> 8),
                                         (u8)len};
    if (probe_write_all(1, hdr, sizeof hdr) < 0)
        return;
    u8 b = 'x';
    i64 end = os_now_ns(OS_CLOCK_MONOTONIC) + 30LL * 1000000000LL;
    while (os_now_ns(OS_CLOCK_MONOTONIC) < end) {
        if (probe_write_all(1, &b, 1) < 0)
            return; /* host closed the pipe */
        os_sleep_ns(50000000); /* 50 ms */
    }
}

/* ---------------------------------------------------------------- framing */

static int probe_write_all(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n > 0) {
        int w = os_write(fd, q, n);
        if (w > 0) {
            q += w;
            n -= (size_t)w;
            continue;
        }
        if (w == 0)
            return -32; /* EPIPE */
        if (w != -4 && w != -11)
            return w;
        struct os_pollfd f = {fd, OS_POLLOUT, 0};
        int pr = os_poll(&f, 1, 1000);
        if (pr < 0 && pr != -4)
            return pr;
    }
    return 0;
}

static int probe_read_exact(int fd, void *buf, size_t n) {
    u8 *q = buf;
    size_t done = 0;
    while (done < n) {
        int r = os_read(fd, q + done, n - done);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r == 0)
            return -32; /* EOF */
        if (r != -4 && r != -11)
            return r;
        struct os_pollfd f = {fd, OS_POLLIN, 0};
        int pr = os_poll(&f, 1, 1000);
        if (pr < 0 && pr != -4)
            return pr;
    }
    return 0;
}

static int probe_write_frame(const void *p, size_t n) {
    AgcBuf f = {0};
    int rc = agentc_frame_encode(&f, p, n);
    if (rc < 0) {
        agentc_buf_free(&f);
        return rc;
    }
    rc = probe_write_all(1, f.p, f.len);
    agentc_buf_free(&f);
    return rc;
}

static int probe_read_frame(char **out, size_t *out_len) {
    u8 hdr[AGENTC_FRAME_HEADER_BYTES];
    int rc = probe_read_exact(0, hdr, sizeof hdr);
    if (rc < 0)
        return rc;
    u32 len = ((u32)hdr[0] << 24) | ((u32)hdr[1] << 16) | ((u32)hdr[2] << 8) | (u32)hdr[3];
    if (len > AGENTC_FRAME_MAX_BYTES)
        return -90; /* EMSGSIZE */
    char *p = agentc_alloc(len + 1);
    if (len) {
        rc = probe_read_exact(0, p, len);
        if (rc < 0) {
            agentc_free(p);
            return rc;
        }
    }
    p[len] = 0;
    *out = p;
    if (out_len)
        *out_len = len;
    return 0;
}

static void send_response(u64 id, bool ok, const char *result_json, const char *error) {
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, id);
    agentc_jsonw_key(&w, "ok");
    agentc_jsonw_bool(&w, ok);
    agentc_jsonw_key(&w, "result");
    if (result_json)
        agentc_jsonw_raw(&w, result_json, agentc_strlen(result_json));
    else
        agentc_jsonw_null(&w);
    agentc_jsonw_key(&w, "error");
    agentc_jsonw_cstr(&w, error ? error : "");
    agentc_jsonw_end(&w);
    (void)probe_write_frame(b.p, b.len);
    agentc_buf_free(&b);
}

/* result is a JSON string containing `text` */
static void send_text_result(u64 id, const char *text) {
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_str(&w, text, agentc_strlen(text));
    send_response(id, true, (const char *)b.p, NULL);
    agentc_buf_free(&b);
}

static void handle_request(const char *req, size_t n) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(a, req, n);
    if (agentc_json_type(o) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(a);
        return;
    }
    i64 id = agentc_json_get_int(o, "id", -1);
    const char *op = agentc_json_get_str(o, "op");
    if (id < 0 || !op) {
        agentc_json_arena_free(a);
        return;
    }
    const AgcJson *payload = agentc_json_get(o, "payload");
    if (agentc_streq(op, "shutdown")) {
        send_response((u64)id, true, NULL, NULL);
        g_shutdown = true;
    } else if (agentc_streq(op, "tool")) {
        const char *name = agentc_json_get_str(o, "name");
        if (g_stall && name && agentc_streq(name, "helper_stall")) {
            probe_stall_frame();
        } else if (name && agentc_streq(name, "helper_echo")) {
            const char *text = agentc_json_get_str(payload, "text");
            if (!text)
                send_response((u64)id, false, NULL, "missing required field: text");
            else
                send_text_result((u64)id, text);
        } else {
            send_response((u64)id, false, NULL, "unknown helper tool");
        }
    } else if (agentc_streq(op, "hook")) {
        const char *point = agentc_json_get_str(o, "point");
        if (point && agentc_streq(point, "tool_call")) {
            const AgcJson *input = agentc_json_get(payload, "input");
            if (input && agentc_json_get_bool(input, "helper_block", false))
                send_response((u64)id, true, "{\"block\":true,\"reason\":\"helper says no\"}", NULL);
            else
                send_response((u64)id, true, "{\"helper_alive\":true}", NULL);
        } else if (point && agentc_streq(point, "agent_end")) {
            send_response((u64)id, true, "{\"helper_observed\":true}", NULL);
        } else {
            send_response((u64)id, true, NULL, NULL);
        }
    } else if (agentc_streq(op, "command")) {
        const char *name = agentc_json_get_str(o, "name");
        if (name && agentc_streq(name, "helper_ping"))
            send_text_result((u64)id, "pong");
        else
            send_response((u64)id, false, NULL, "unknown helper command");
    } else {
        send_response((u64)id, false, NULL, "unknown op");
    }
    agentc_json_arena_free(a);
}

int agentc_main(int argc, char **argv) {
    bool stall = argc > 2 && argv[1] != NULL && agentc_streq(argv[1], "--stall");
    g_stall = stall;
    if (stall) {
        if (probe_write_frame(g_stall_manifest, sizeof g_stall_manifest - 1) < 0)
            return 1;
        /* Record our pid so the host can assert the reaped process is gone. */
        char pidbuf[32];
        agentc_snprintf(pidbuf, sizeof pidbuf, "%d\n", os_getpid());
        int fd = os_open(argv[2], OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
        if (fd >= 0) {
            (void)os_write(fd, pidbuf, agentc_strlen(pidbuf));
            os_close(fd);
        }
        (void)os_sig_install(OS_SIGTERM, sig_noop);
    } else {
        if (probe_write_frame(g_manifest, sizeof g_manifest - 1) < 0)
            return 1;
    }
    while (!g_shutdown) {
        char *req = NULL;
        size_t n = 0;
        if (probe_read_frame(&req, &n) < 0)
            break; /* host closed the pipe */
        handle_request(req, n);
        agentc_free(req);
    }
    return 0;
}
