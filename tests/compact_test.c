/* compact_test.c — context estimation, cut boundaries, manual and automatic
 * compaction against a replaying transport (streamed summary request with the
 * event hub quiet).
 */
#include "agent.h"
#include "session.h"
#include "ext.h"

/* internal test hooks / helpers (not in the frozen headers) */
void agentc_agent_test_no_backoff(AgcAgent *a);
void agentc_msg_add_tool_result(AgcMsg *m, const char *call_id, const char *name,
                            const char *result);
u32 agentc_compact_estimate(const AgcTranscript *tr);
size_t agentc_compact_cut(const AgcTranscript *tr, u32 keep_recent_tokens);
void agentc_rm_rf(const char *path);
char *agentc_read_file_owned(const char *path, size_t *len);

#define CROOT "/tmp/agentc-compact-test"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

/* ----------------------------------------------------- replay transport */

typedef struct {
    int codes[8];
    const char *bodies[8];
    size_t n, i;
    AgcBuf last_body;
} Replay;

static int replay_request(void *ud, const char *url, const char *headers, const void *body,
                          size_t body_len, int (*on_chunk)(void *, const void *, size_t),
                          void *u, int timeout_ms, AgcBuf *record) {
    (void)url;
    (void)headers;
    (void)timeout_ms;
    (void)record;
    Replay *r = ud;
    agentc_buf_clear(&r->last_body);
    agentc_buf_push(&r->last_body, body, body_len);
    if (r->i >= r->n) return -5;
    int code = r->codes[r->i];
    const char *resp = r->bodies[r->i];
    r->i++;
    if (code != 0) return code;
    if (resp) {
        size_t len = agentc_strlen(resp);
        for (size_t off = 0; off < len; off += 13) {
            size_t n = len - off;
            if (n > 13) n = 13;
            if (on_chunk(u, resp + off, n)) return -125;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ fixtures */

static void push_user(AgcTranscript *t, const char *s) {
    AgcMsg *m = agentc_transcript_push(t, AGENTC_ROLE_USER);
    agentc_msg_add_text(m, s, agentc_strlen(s));
}

static void push_asst(AgcTranscript *t, u32 in, u32 out) {
    AgcMsg *m = agentc_transcript_push(t, AGENTC_ROLE_ASSISTANT);
    m->usage.input = in;
    m->usage.output = out;
    agentc_msg_add_text(m, "ok", 2);
    m->stop_reason = AGENTC_STOP_STOP;
}

static char *repeat_char(size_t n, char c) {
    char *s = agentc_alloc(n + 1);
    agentc_memset(s, c, n);
    s[n] = 0;
    return s;
}

static const char summary_text[] =
    "## Goal\nsummary-auto\n## Constraints\nnone\n## Progress\nstep\n"
    "## Decisions\ndecided\n## Next Steps\nnext\n## Critical Context\nctx";

/* The same summary, now delivered as an Anthropic SSE stream: compaction runs
 * through the ordinary stream path and concatenates the text blocks. */
static const char summary_sse[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_sum\",\"usage\":{"
    "\"input_tokens\":10,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"## Goal\\nsummary-auto\\n## Constraints\\nnone\\n"
    "## Progress\\nstep\\n## Decisions\\ndecided\\n## Next Steps\\nnext\\n"
    "## Critical Context\\nctx\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":20}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_final[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_2\",\"usage\":{"
    "\"input_tokens\":8,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"done\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* -------------------------------------------------------------- events */

typedef struct {
    int compacts;
    u32 tokens_before;
    u32 kept;
    bool automatic;
    AgcBuf summary;
    u32 deltas;                  /* TEXT_DELTA events seen so far */
    u32 deltas_at_compact;       /* of those, seen when COMPACT was emitted */
    const AgcAgent *agent;    /* set when the test wants transcript_n_at_compact */
    AgcSession *session;      /* set when the record should persist on emit */
    int append_rc;
    u32 transcript_n_at_compact;
} CompactEvents;

static void collector(void *ud, int ev, const void *data) {
    CompactEvents *c = ud;
    if (ev == AGENTC_EV_TEXT_DELTA) {
        c->deltas++;
        return;
    }
    if (ev != AGENTC_EV_COMPACT) return;
    const AgcCompactInfo *ci = data;
    c->compacts++;
    c->tokens_before = ci->tokens_before;
    c->kept = ci->kept_messages;
    c->automatic = ci->automatic;
    if (ci->summary) {
        agentc_buf_clear(&c->summary);
        agentc_buf_cstr(&c->summary, ci->summary);
    }
    c->deltas_at_compact = c->deltas;
    if (c->agent) c->transcript_n_at_compact = (u32)agentc_agent_transcript(c->agent)->n;
    if (c->session) c->append_rc = agentc_session_append_compaction(c->session, ci);
}

/* ------------------------------------------------------ direct hook recorder */

typedef struct {
    int calls;
    AgcBuf payload;
    const char *result;
    int result_calls;
} HookRec;

static int hook_rec_fn(void *ud, const char *point, const char *payload_json,
                       char **result_json) {
    (void)point;
    HookRec *h = ud;
    h->calls++;
    agentc_buf_clear(&h->payload);
    if (payload_json) agentc_buf_cstr(&h->payload, payload_json);
    if (result_json && h->result && h->calls <= h->result_calls)
        *result_json = agentc_strdup(h->result);
    return 0;
}

static uint64_t hook_on(const char *point, uint32_t caps, HookRec *h) {
    return agentc_ext_host()->on(point, caps, 0, hook_rec_fn, h);
}

static void hook_clear(HookRec *h) {
    agentc_buf_free(&h->payload);
    agentc_memset(h, 0, sizeof *h);
}

/* ---------------------------------------------------------------- tests */

static void test_estimate(void) {
    AgcTranscript t;
    agentc_transcript_init(&t);
    t.system = agentc_strdup("sys");
    push_user(&t, "1234567890123456789012345678901234567890");   /* 40 chars */
    push_asst(&t, 1000, 200);
    char *big = repeat_char(400, 'x');
    push_user(&t, big);
    agentc_free(big);

    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    check("estimate_load", agentc_agent_load(a, &t) == 0);
    /* 1200 + (400 + 8 + 3)/4 */
    check("estimate_usage", agentc_agent_context_tokens(a) == 1302);
    agentc_transcript_free(&t);
    agentc_agent_free(a);

    AgcTranscript t2;
    agentc_transcript_init(&t2);
    t2.system = agentc_strdup("abcd");                              /* 1 token */
    push_user(&t2, "12345678");                                 /* (8+8+3)/4 = 4 */
    check("estimate_no_usage", agentc_compact_estimate(&t2) == 5);
    agentc_transcript_free(&t2);
}

static void test_cut(void) {
    AgcTranscript t;
    agentc_transcript_init(&t);
    char *big = repeat_char(4000, 'a');
    push_user(&t, big);                       /* ~1002 tokens */
    agentc_free(big);
    push_asst(&t, 10, 10);                    /* text 2 + 8 + ... small */
    AgcMsg *tc = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_tool_call(tc, "c1", "read");
    agentc_msg_tool_args_append(tc, "{\"path\":\"x\"}", 12);
    AgcMsg *tr = agentc_transcript_push(&t, AGENTC_ROLE_TOOL);
    agentc_msg_add_tool_result(tr, "c1", "read", "0123456789012345678901234567890123456789");

    check("cut_keep_all", agentc_compact_cut(&t, 100000) == 0);
    /* only the tool result fits -> the cut must back up to its call */
    size_t c = agentc_compact_cut(&t, 20);
    check("cut_boundary", c == 2 && t.msgs[c].role == AGENTC_ROLE_ASSISTANT);
    /* the trailing user message fits alone */
    push_user(&t, "tail");
    c = agentc_compact_cut(&t, 10);
    check("cut_user", c == 4 && t.msgs[c].role == AGENTC_ROLE_USER);
    agentc_transcript_free(&t);
}

static void test_manual_compact(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = summary_sse;
    AgcTransport tr = { replay_request, &r, NULL };

    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, tr);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    agentc_agent_set_compact_limits(a, 4096, 50);
    CompactEvents ev;
    agentc_memset(&ev, 0, sizeof ev);
    agentc_agent_set_events(a, collector, &ev);

    AgcTranscript t;
    agentc_transcript_init(&t);
    char *big = repeat_char(400, 'u');
    push_user(&t, big);                       /* ~102 tokens */
    agentc_free(big);
    push_asst(&t, 10, 10);
    AgcMsg *tc = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_tool_call(tc, "c1", "read");
    AgcMsg *tm = agentc_transcript_push(&t, AGENTC_ROLE_TOOL);
    agentc_msg_add_tool_result(tm, "c1", "read", "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
    push_user(&t, "tail");
    check("manual_load", agentc_agent_load(a, &t) == 0);
    agentc_transcript_free(&t);

    u32 before = agentc_agent_context_tokens(a);
    check("manual_rc", agentc_agent_compact(a) == 0);
    check("manual_requests", r.i == 1);
    check("manual_event", ev.compacts == 1 && !ev.automatic && ev.kept == 4 &&
                             ev.tokens_before == before);
    check("manual_summary",
          ev.summary.p && agentc_streq((const char *)ev.summary.p, summary_text));
    check("manual_compacted_flag", agentc_agent_compacted(a));

    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("manual_shape", tp && tp->n == 5 && tp->msgs[0].role == AGENTC_ROLE_USER &&
                              tp->msgs[4].role == AGENTC_ROLE_USER &&
                              agentc_streq(tp->msgs[4].blocks[0].text, "tail"));
    check("manual_summary_msg", tp && tp->msgs[0].nblocks == 1 &&
                                    contains(tp->msgs[0].blocks[0].text, "## Goal"));
    check("manual_request_stream",
          contains((const char *)r.last_body.p, "\"stream\":true"));
    check("manual_request_max_tokens",
          contains((const char *)r.last_body.p, "\"max_tokens\":3276"));
    check("manual_request_checkpoint_prompt",
          contains((const char *)r.last_body.p, "## Next Steps"));
    /* the streamed summary ran with the event hub quiet: no turn deltas */
    check("manual_quiet", ev.deltas == 0 && ev.deltas_at_compact == 0);

    agentc_buf_free(&ev.summary);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_auto_compact(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.bodies[0] = summary_sse;
    r.bodies[1] = reply_final;
    AgcTransport tr = { replay_request, &r, NULL };

    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, tr);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    agentc_agent_set_compact_limits(a, 16384, 10);
    agentc_agent_set_auto_compact(a, true);
    CompactEvents ev;
    agentc_memset(&ev, 0, sizeof ev);
    agentc_agent_set_events(a, collector, &ev);

    AgcTranscript t;
    agentc_transcript_init(&t);
    char *big = repeat_char(4000, 'p');
    push_user(&t, big);                     /* gives the cut point room to move */
    agentc_free(big);
    push_asst(&t, 200000, 100);              /* far beyond 200k - 16384 */
    check("auto_load", agentc_agent_load(a, &t) == 0);
    agentc_transcript_free(&t);

    int rc = agentc_agent_submit(a, "go");
    check("auto_rc", rc == 0);
    check("auto_requests", r.i == 2);
    check("auto_event", ev.compacts == 1 && ev.automatic && ev.kept == 2 &&
                            ev.tokens_before > 0);
    check("auto_summary", ev.summary.p && contains((const char *)ev.summary.p, "summary-auto"));
    check("auto_summary_replayed",
          contains((const char *)r.last_body.p, "summary-auto"));
    /* no deltas while the summary streamed; the final turn's deltas follow */
    check("auto_quiet", ev.deltas_at_compact == 0 && ev.deltas > ev.deltas_at_compact);

    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("auto_shape", tp && tp->n == 4 && tp->msgs[0].role == AGENTC_ROLE_USER &&
                            agentc_streq(tp->msgs[3].blocks[0].text, "done"));
    agentc_buf_free(&ev.summary);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);

    /* below threshold: no compaction request */
    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 1;
    r2.bodies[0] = reply_final;
    AgcTransport tr2 = { replay_request, &r2, NULL };
    AgcAgent *b = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(b, tr2);
    agentc_agent_set_system(b, "sys");
    agentc_agent_test_no_backoff(b);
    agentc_agent_set_auto_compact(b, true);
    CompactEvents ev2;
    agentc_memset(&ev2, 0, sizeof ev2);
    agentc_agent_set_events(b, collector, &ev2);
    check("auto_below_rc", agentc_agent_submit(b, "hello") == 0);
    check("auto_below_none", ev2.compacts == 0 && r2.i == 1);
    agentc_agent_free(b);
}

/* Join the value of every "type" field, in file order, as "a,b,c". Each
 * byte buffer is modified in place (newlines become NULs). */
static void line_type_trace(char *text, size_t len, AgcBuf *out) {
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') text[i] = 0;
    const char *p = text;
    while (*p) {
        const char *t = agentc_str_str(p, "\"type\":\"");
        if (!t) break;
        t += 8;
        const char *e = t;
        while (*e && *e != '"') e++;
        if (out->len) agentc_buf_byte(out, ',');
        agentc_buf_push(out, t, (size_t)(e - t));
        p += agentc_strlen(p) + 1;
    }
    agentc_buf_byte(out, 0);
}

/* A hand-written compaction record prepends its summary to the last
 * `kept_messages` messages of the replay and drops the compacted prefix; the
 * system prompt is not part of it. Here kept_messages=1 retains the assistant
 * "two" that precedes the record. */
static void test_replay(void) {
    agentc_rm_rf(CROOT);
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = CROOT;
    o.dir = CROOT "/replay";
    o.id = "replay01";
    AgcSession *s = agentc_session_new(&o);
    static const char *lines[] = {
        "{\"type\":\"message\",\"role\":\"user\",\"ts\":1,\"content\":[{\"type\":\"text\",\"text\":\"one\"}]}\n",
        "{\"type\":\"message\",\"role\":\"assistant\",\"ts\":2,\"content\":[{\"type\":\"text\",\"text\":\"two\"}]}\n",
        "{\"type\":\"compaction\",\"ts\":3,\"tokens_before\":9,\"kept_messages\":1,\"automatic\":false,\"summary\":\"checkpoint\"}\n",
        "{\"type\":\"message\",\"role\":\"user\",\"ts\":4,\"content\":[{\"type\":\"text\",\"text\":\"after\"}]}\n",
    };
    bool wrote = s != NULL;
    for (size_t i = 0; i < 4 && wrote; i++)
        wrote = agentc_session_append_raw(s, lines[i], agentc_strlen(lines[i])) == 0;
    check("compact.replay.write", wrote);
    char *rpath = s ? agentc_strdup(agentc_session_path(s)) : NULL;
    agentc_session_close(s);

    AgcSession *r = agentc_session_open(rpath);
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    tr.system = agentc_strdup("system-stays");
    check("compact.replay.open", r != NULL);
    check("compact.replay.load",
          r != NULL && agentc_session_load_messages(r, &tr) == 0);
    check("compact.replay.reset",
          tr.n == 3 && tr.msgs[0].role == AGENTC_ROLE_USER &&
              agentc_streq(tr.msgs[0].blocks[0].text, "checkpoint") &&
              agentc_streq(tr.msgs[1].blocks[0].text, "two") &&
              agentc_streq(tr.msgs[2].blocks[0].text, "after"));
    check("compact.replay.system_kept", agentc_streq(tr.system, "system-stays"));
    agentc_transcript_free(&tr);
    agentc_session_close(r);
    agentc_free(rpath);
}

/* A checkpoint whose kept_messages covers the whole replay: drop==0 and
 * keep==cap. The capacity must grow before the tail is shifted right, or the
 * last kept message is written one past the array. */
static void test_replay_keepall(void) {
    agentc_rm_rf(CROOT);
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = CROOT;
    o.dir = CROOT "/keepall";
    o.id = "keepall01";
    AgcSession *s = agentc_session_new(&o);
    AgcTranscript t;
    agentc_transcript_init(&t);
    bool ok = s != NULL;
    char line[256];
    for (size_t i = 0; ok && i < 8; i++) {
        int n = agentc_snprintf(line, sizeof line,
                           "{\"type\":\"message\",\"role\":\"user\",\"ts\":%u,"
                           "\"content\":[{\"type\":\"text\",\"text\":\"m%u\"}]}\n",
                           (unsigned)(i + 1), (unsigned)i);
        ok = n > 0 && agentc_session_append_raw(s, line, (size_t)n) == 0;
    }
    static const char comp[] =
        "{\"type\":\"compaction\",\"ts\":9,\"tokens_before\":9,\"kept_messages\":8,"
        "\"automatic\":false,\"summary\":\"cp\"}\n";
    ok = ok && agentc_session_append_raw(s, comp, agentc_strlen(comp)) == 0;
    char *cpath = s ? agentc_strdup(agentc_session_path(s)) : NULL;
    agentc_session_close(s);

    AgcSession *r = agentc_session_open(cpath);
    ok = ok && r != NULL && agentc_session_load_messages(r, &t) == 0;
    check("compact.keepall.load", ok);
    check("compact.keepall.shape",
          t.n == 9 && agentc_streq(t.msgs[0].blocks[0].text, "cp") &&
              agentc_streq(t.msgs[8].blocks[0].text, "m7"));
    agentc_transcript_free(&t);
    agentc_session_close(r);
    agentc_free(cpath);
}

/* The COMPACT event fires before the transcript splice; the front end persists
 * the record at that moment, and the next append lands after it. */
static void test_record_before_splice(void) {
    agentc_rm_rf(CROOT);
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = summary_sse;
    AgcTransport tr = { replay_request, &r, NULL };

    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, tr);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    agentc_agent_set_compact_limits(a, 4096, 5);

    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = CROOT;
    o.dir = CROOT "/sessions";
    o.id = "compact1";
    AgcSession *s = agentc_session_new(&o);
    char *spath = s ? agentc_strdup(agentc_session_path(s)) : NULL;
    check("compact.record.session_new", s != NULL && spath != NULL);

    CompactEvents ev;
    agentc_memset(&ev, 0, sizeof ev);
    ev.agent = a;
    ev.session = s;
    agentc_agent_set_events(a, collector, &ev);

    AgcTranscript t;
    agentc_transcript_init(&t);
    push_asst(&t, 1000, 200);
    push_asst(&t, 1000, 200);
    push_user(&t, "tail");
    check("compact.record.load", agentc_agent_load(a, &t) == 0);
    agentc_transcript_free(&t);

    const AgcTranscript *pre = agentc_agent_transcript(a);
    check("compact.record.pre_count", pre != NULL && pre->n == 3);
    check("compact.record.rc", agentc_agent_compact(a) == 0);
    check("compact.record.requests", r.i == 1);
    check("compact.record.event", ev.compacts == 1 && ev.kept == 1);
    check("compact.record.emitted_before_splice", ev.transcript_n_at_compact == 3);
    check("compact.record.persisted", ev.append_rc == 0);

    const AgcTranscript *post = agentc_agent_transcript(a);
    check("compact.record.post_count",
          post != NULL && post->n == 2 &&
              agentc_streq(post->msgs[1].blocks[0].text, "tail"));

    /* The observer/flush pattern: persist the new message after the splice. */
    check("compact.record.followup_append",
          agentc_session_append_message(s, &post->msgs[post->n - 1]) == 0);
    agentc_session_close(s);

    size_t flen = 0;
    char *ftext = agentc_read_file_owned(spath, &flen);
    check("compact.record.followup_is_tail",
          ftext != NULL && agentc_str_str(ftext, "\"text\":\"tail\"") != NULL);
    AgcBuf types = { 0 };
    if (ftext) line_type_trace(ftext, flen, &types);
    check("compact.record.session_order",
          types.p != NULL && agentc_streq((const char *)types.p, "session,compaction,message"));
    agentc_free(ftext);
    agentc_buf_free(&types);
    agentc_free(spath);
    agentc_agent_free(a);
}

/* ---------------------------------------------------------- compact hooks */

/* The same fixture the manual test uses: five messages, cut == 1, kept == 4. */
static void load_compact_fixture(AgcAgent *a) {
    AgcTranscript t;
    agentc_transcript_init(&t);
    char *big = repeat_char(400, 'u');
    push_user(&t, big);
    agentc_free(big);
    push_asst(&t, 10, 10);
    AgcMsg *tc = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_tool_call(tc, "c1", "read");
    AgcMsg *tm = agentc_transcript_push(&t, AGENTC_ROLE_TOOL);
    agentc_msg_add_tool_result(
        tm, "c1", "read",
        "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
    push_user(&t, "tail");
    check("hook_fixture_load", agentc_agent_load(a, &t) == 0);
    agentc_transcript_free(&t);
}

static AgcAgent *compact_agent(AgcTransport tr) {
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, tr);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    agentc_agent_set_compact_limits(a, 4096, 50);
    return a;
}

static void test_hook_cancel(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = summary_sse;
    AgcTransport tr = { replay_request, &r, NULL };
    AgcAgent *a = compact_agent(tr);
    CompactEvents ev;
    agentc_memset(&ev, 0, sizeof ev);
    agentc_agent_set_events(a, collector, &ev);
    HookRec before, done, failed;
    agentc_memset(&before, 0, sizeof before);
    agentc_memset(&done, 0, sizeof done);
    agentc_memset(&failed, 0, sizeof failed);
    before.result = "{\"cancel\":true}";
    before.result_calls = 1;
    uint64_t hb = hook_on("session_before_compact", AGENTC_HOOK_OVERRIDE, &before);
    uint64_t hd = hook_on("session_compact", AGENTC_HOOK_OBSERVE, &done);
    uint64_t hf = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &failed);

    load_compact_fixture(a);
    u32 tokens = agentc_agent_context_tokens(a);
    check("hook_cancel_rc", agentc_agent_compact(a) == 0);
    check("hook_cancel_no_request", r.i == 0);
    check("hook_cancel_no_event", ev.compacts == 0);
    check("hook_cancel_no_done_failed", done.calls == 0 && failed.calls == 0);
    check("hook_cancel_payload",
          before.calls == 1 &&
              contains((const char *)before.payload.p, "\"reason\":\"manual\"") &&
              contains((const char *)before.payload.p, "\"first_kept\":1") &&
              contains((const char *)before.payload.p, "\"messages\":[") &&
              contains((const char *)before.payload.p, "\"role\":\"user\""));
    check("hook_cancel_transcript",
          agentc_agent_context_tokens(a) == tokens &&
              agentc_agent_transcript(a)->n == 5);

    agentc_ext_host()->off(hb);
    agentc_ext_host()->off(hd);
    agentc_ext_host()->off(hf);
    hook_clear(&before);
    hook_clear(&done);
    hook_clear(&failed);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_hook_ext_summary(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = summary_sse;
    AgcTransport tr = { replay_request, &r, NULL };
    AgcAgent *a = compact_agent(tr);
    CompactEvents ev;
    agentc_memset(&ev, 0, sizeof ev);
    agentc_agent_set_events(a, collector, &ev);
    HookRec before, done, failed;
    agentc_memset(&before, 0, sizeof before);
    agentc_memset(&done, 0, sizeof done);
    agentc_memset(&failed, 0, sizeof failed);
    before.result = "{\"summary\":\"EXT-SUMMARY\"}";
    before.result_calls = 1;
    uint64_t hb = hook_on("session_before_compact", AGENTC_HOOK_OVERRIDE, &before);
    uint64_t hd = hook_on("session_compact", AGENTC_HOOK_OBSERVE, &done);
    uint64_t hf = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &failed);

    load_compact_fixture(a);
    check("hook_sum_rc", agentc_agent_compact(a) == 0);
    check("hook_sum_no_request", r.i == 0);
    check("hook_sum_event", ev.compacts == 1 && !ev.automatic && ev.kept == 4);
    check("hook_sum_summary",
          ev.summary.p && agentc_streq((const char *)ev.summary.p, "EXT-SUMMARY"));
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("hook_sum_shape",
          tp && tp->n == 5 && agentc_streq(tp->msgs[0].blocks[0].text, "EXT-SUMMARY") &&
              agentc_streq(tp->msgs[4].blocks[0].text, "tail"));
    check("hook_sum_done",
          done.calls == 1 &&
              agentc_streq((const char *)done.payload.p,
                           "{\"reason\":\"manual\",\"from_extension\":true}"));
    check("hook_sum_failed_none", failed.calls == 0);
    check("hook_sum_messages",
          before.calls == 1 &&
              contains((const char *)before.payload.p,
                       "\"content\":[{\"type\":\"text\",\"text\":\"uuuu"));

    agentc_ext_host()->off(hb);
    agentc_ext_host()->off(hd);
    agentc_ext_host()->off(hf);
    hook_clear(&before);
    hook_clear(&done);
    hook_clear(&failed);
    agentc_buf_free(&ev.summary);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_hook_done_normal(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = summary_sse;
    AgcTransport tr = { replay_request, &r, NULL };
    AgcAgent *a = compact_agent(tr);
    HookRec before, done, failed;
    agentc_memset(&before, 0, sizeof before);
    agentc_memset(&done, 0, sizeof done);
    agentc_memset(&failed, 0, sizeof failed);
    /* A result without cancel/summary is not a decision: default compaction. */
    before.result = "{\"cancel\":false}";
    before.result_calls = 1;
    uint64_t hb = hook_on("session_before_compact", AGENTC_HOOK_OVERRIDE, &before);
    uint64_t hd = hook_on("session_compact", AGENTC_HOOK_OBSERVE, &done);
    uint64_t hf = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &failed);

    load_compact_fixture(a);
    check("hook_done_rc", agentc_agent_compact(a) == 0);
    check("hook_done_request", r.i == 1);
    check("hook_done_payload",
          done.calls == 1 &&
              agentc_streq((const char *)done.payload.p,
                           "{\"reason\":\"manual\",\"from_extension\":false}"));
    check("hook_done_failed_none", failed.calls == 0);

    agentc_ext_host()->off(hb);
    agentc_ext_host()->off(hd);
    agentc_ext_host()->off(hf);
    hook_clear(&before);
    hook_clear(&done);
    hook_clear(&failed);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_hook_failed(void) {
    /* request failure */
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 500;
    AgcTransport tr = { replay_request, &r, NULL };
    AgcAgent *a = compact_agent(tr);
    CompactEvents ev;
    agentc_memset(&ev, 0, sizeof ev);
    agentc_agent_set_events(a, collector, &ev);
    HookRec done, failed;
    agentc_memset(&done, 0, sizeof done);
    agentc_memset(&failed, 0, sizeof failed);
    uint64_t hd = hook_on("session_compact", AGENTC_HOOK_OBSERVE, &done);
    uint64_t hf = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &failed);

    load_compact_fixture(a);
    check("hook_fail_rc", agentc_agent_compact(a) != 0);
    check("hook_fail_request", r.i == 1);
    check("hook_fail_event", ev.compacts == 0);
    check("hook_fail_payload",
          failed.calls == 1 &&
              agentc_streq((const char *)failed.payload.p,
                           "{\"reason\":\"manual\",\"aborted\":false,\"error\":"
                           "\"compaction: summarization request failed\"}"));
    check("hook_fail_done_none", done.calls == 0);

    agentc_ext_host()->off(hd);
    agentc_ext_host()->off(hf);
    hook_clear(&done);
    hook_clear(&failed);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);

    /* no transport: a real failure that fires before any request */
    AgcAgent *b = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_system(b, "sys");
    agentc_agent_set_compact_limits(b, 4096, 50);
    HookRec f2;
    agentc_memset(&f2, 0, sizeof f2);
    uint64_t fh2 = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &f2);
    check("hook_fail_notransport_rc", agentc_agent_compact(b) == -1);
    check("hook_fail_notransport_payload",
          f2.calls == 1 && contains((const char *)f2.payload.p, "\"reason\":\"manual\"") &&
              contains((const char *)f2.payload.p, "\"aborted\":false") &&
              contains((const char *)f2.payload.p, "\"error\":\"\""));
    agentc_ext_host()->off(fh2);
    hook_clear(&f2);
    agentc_agent_free(b);

    /* no provider: also a real failure */
    AgcAgent *d = agentc_agent_new(NULL, "model");
    HookRec f4;
    agentc_memset(&f4, 0, sizeof f4);
    uint64_t fh4 = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &f4);
    check("hook_fail_noprov_rc", agentc_agent_compact(d) == -1);
    check("hook_fail_noprov_payload",
          f4.calls == 1 && contains((const char *)f4.payload.p, "\"reason\":\"manual\"") &&
              contains((const char *)f4.payload.p, "\"aborted\":false"));
    agentc_ext_host()->off(fh4);
    hook_clear(&f4);
    agentc_agent_free(d);

    /* nothing to do: no failed event */
    Replay r3;
    agentc_memset(&r3, 0, sizeof r3);
    r3.n = 1;
    AgcTransport tr3 = { replay_request, &r3, NULL };
    AgcAgent *c = compact_agent(tr3);
    HookRec f3;
    agentc_memset(&f3, 0, sizeof f3);
    uint64_t fh3 = hook_on("session_compact_failed", AGENTC_HOOK_OBSERVE, &f3);
    check("hook_fail_nothing_rc", agentc_agent_compact(c) == 0);
    check("hook_fail_nothing_none", f3.calls == 0 && r3.i == 0);
    agentc_ext_host()->off(fh3);
    hook_clear(&f3);
    agentc_buf_free(&r3.last_body);
    agentc_agent_free(c);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_estimate();
    test_cut();
    test_manual_compact();
    test_auto_compact();
    test_replay();
    test_replay_keepall();
    test_record_before_splice();
    test_hook_cancel();
    test_hook_ext_summary();
    test_hook_done_normal();
    test_hook_failed();
    agentc_rm_rf(CROOT);
    return fails;
}
