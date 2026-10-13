/* session_test.c — JSONL sessions: write -> reopen -> replay, tool args string
 * encoding, memory-only mode, listing order and find_latest. All paths under /tmp. */
#include "agent.h"
#include "session.h"
#include "plat.h"
#include "config.h"
#include "message.h"
#include "core/runsession.h"
#include "core/events.h"

/* internal helpers (not in the frozen headers) */
void agentc_msg_add_think(AgcMsg *m);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
void agentc_msg_add_tool_result(AgcMsg *m, const char *call_id, const char *name,
                                const char *result);
char *agentc_read_file_owned(const char *path, size_t *len);
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);
void agentc_rm_rf(const char *path);
bool agentc_path_is_dir(const char *path);
int agentc_mkdir_parents(const char *path);
void agentc_session_test_fail_next_append(AgcSession *s);
void agentc_session_test_fail_next_truncate(bool fail);

#define ROOT "/tmp/agentc-session-test"
#define DATA ROOT "/data"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = agentc_strlen(s), fl = agentc_strlen(suffix);
    return sl >= fl && agentc_memeq(s + sl - fl, suffix, fl);
}

/* Write the whole file, truncating any previous contents. */
static bool write_raw(const char *path, const char *data, size_t n) {
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0600);
    if (fd < 0)
        return false;
    while (n) {
        int w = os_write(fd, data, n);
        if (w <= 0) {
            os_close(fd);
            return false;
        }
        data += w;
        n -= (size_t)w;
    }
    os_close(fd);
    return true;
}

/* Append raw bytes (no newline added): simulates a torn or foreign write. */
static bool append_raw_file(const char *path, const char *data, size_t n) {
    int fd = os_open(path, OS_O_WRONLY | OS_O_APPEND, 0600);
    if (fd < 0)
        return false;
    while (n) {
        int w = os_write(fd, data, n);
        if (w <= 0) {
            os_close(fd);
            return false;
        }
        data += w;
        n -= (size_t)w;
    }
    os_close(fd);
    return true;
}

static void build_messages(AgcTranscript *tr) {
    AgcMsg *u = agentc_transcript_push(tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(u, "hello world", 11);

    AgcMsg *a = agentc_transcript_push(tr, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_think(a);
    agentc_msg_block_append(&a->blocks[0], "thinking hard", 13);
    agentc_msg_add_text(a, "answer", 6);
    agentc_msg_add_tool_call(a, "call_1", "read");
    const char *raw = "{ \"path\" : \"a b.txt\" }";
    agentc_msg_tool_args_append(a, raw, agentc_strlen(raw));
    a->stop_reason = AGENTC_STOP_TOOLUSE;

    AgcMsg *t = agentc_transcript_push(tr, AGENTC_ROLE_TOOL);
    agentc_msg_add_tool_result(t, "call_1", "read", "file body\n");
    t->error = agentc_strdup("error");
}

static void test_fork_tree(void) {
    char dir[512];
    agentc_snprintf(dir, sizeof dir, "%s/forktree", ROOT);
    (void)agentc_mkdir_parents(dir);
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = ROOT "/proj";
    o.dir = dir;
    o.id = "root0001";
    AgcSession *root = agentc_session_new(&o);
    check("session.fork.root_create", root != NULL && agentc_session_parent(root) == NULL);
    char rootid[16];
    agentc_snprintf(rootid, sizeof rootid, "%s", agentc_session_id(root));
    agentc_session_close(root);

    o.id = "child001";
    o.parent_session = rootid;
    AgcSession *child = agentc_session_new(&o);
    check("session.fork.child_parent",
          child != NULL && agentc_session_parent(child) != NULL &&
              agentc_streq(agentc_session_parent(child), rootid));
    char *childpath = agentc_strdup(agentc_session_path(child));
    agentc_session_close(child);

    size_t hlen = 0;
    char *h = agentc_read_file_owned(childpath, &hlen);
    check("session.fork.header", h && contains(h, "\"parent_session\":\"root0001\""));
    agentc_free(h);

    AgcSession *r = agentc_session_open(childpath);
    check("session.fork.reopen",
          r != NULL && agentc_session_parent(r) != NULL &&
              agentc_streq(agentc_session_parent(r), rootid));
    agentc_session_close(r);

    size_t total = agentc_session_tree(dir, NULL, 0);
    check("session.fork.tree_count", total == 2);
    /* a malformed file (bad header) is skipped by the count too, so a caller
     * that sizes by the count never sees an unwritten row. */
    char garbage[600];
    agentc_snprintf(garbage, sizeof garbage, "%s/garbage.jsonl", dir);
    check("session.fork.garbage_write",
          write_raw(garbage, "not a session header\n", 21));
    check("session.fork.tree_count_valid", agentc_session_tree(dir, NULL, 0) == 2);
    AgcSessionNode nodes[4];
    size_t n = agentc_session_tree(dir, nodes, 4);
    bool found_root = false, found_child = false;
    for (size_t i = 0; i < n; i++) {
        if (agentc_streq(nodes[i].id, "root0001"))
            found_root = nodes[i].parent_id == NULL;
        if (agentc_streq(nodes[i].id, "child001"))
            found_child = nodes[i].parent_id && agentc_streq(nodes[i].parent_id, "root0001") &&
                          nodes[i].timestamp_ms > 0;
    }
    check("session.fork.tree_links", n == 2 && found_root && found_child);
    for (size_t i = 0; i < n; i++) {
        agentc_free(nodes[i].path);
        agentc_free(nodes[i].id);
        agentc_free(nodes[i].parent_id);
    }
    agentc_free(childpath);
}

static int count_custom_cb(void *ud, const char *type, const char *data) {
    int *n = ud;
    if (agentc_streq(type, "notice") && data && agentc_str_str(data, "hi") != NULL)
        (*n)++;
    return 0;
}

/* The typed (`agentc_session_load_custom`) callback shape: data only. */
static int count_typed_cb(void *ud, const char *data_json) {
    (void)ud;
    (void)data_json;
    return 0;
}

/* The all-records custom iterator (used by /fork to copy parent custom rows). */
static void test_custom_all(void) {
    char dir[512];
    agentc_snprintf(dir, sizeof dir, "%s/customall", ROOT);
    (void)agentc_mkdir_parents(dir);
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = ROOT "/proj";
    o.dir = dir;
    o.id = "cust0001";
    AgcSession *s = agentc_session_new(&o);
    static const char r1[] =
        "{\"type\":\"custom\",\"custom_type\":\"notice\",\"data\":{\"text\":\"hi\"}}";
    static const char r2[] =
        "{\"type\":\"custom\",\"custom_type\":\"queue\",\"data\":{\"n\":0}}";
    check("session.custom.write",
          s != NULL && agentc_session_append_raw(s, r1, agentc_strlen(r1)) == 0 &&
              agentc_session_append_raw(s, r2, agentc_strlen(r2)) == 0);
    int n = 0;
    check("session.custom.all",
          agentc_session_load_custom_all(s, count_custom_cb, &n) == 0 && n == 1);
    /* B-L4: the typed entry requires a type; a NULL/empty type must not silently
     * scan every custom record (that is _all's job). */
    int n2 = 0;
    check("session.custom.null_type_refused",
          agentc_session_load_custom(s, NULL, count_typed_cb, &n2) == -22 &&
              agentc_session_load_custom(s, "", count_typed_cb, &n2) == -22);
    agentc_session_close(s);
}

/* B-M1: agentc_session_new degrades to a path-less memory-only session when its
 * file cannot be created; the mode layer (runsession.c) must reject that on
 * /new and /fork. This pins the degrade contract the callers guard against. */
static void test_degrade_memory_only(void) {
    (void)agentc_mkdir_parents(ROOT "/degrade/x");
    check("degrade.blocker_file", write_raw(ROOT "/degrade_blocker", "x", 1));
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = ROOT "/proj";
    o.dir = ROOT "/degrade_blocker/sess";
    o.id = "degrade01";
    /* The session name carries a timestamp, so the create warning is
     * non-deterministic; suppress it for this call (the contract under test is
     * the path-less memory-only result, not the log line). */
    agentc_log_set_level(4);
    AgcSession *s = agentc_session_new(&o);
    agentc_log_set_level(-1);
    check("degrade.memory_only_on_bad_dir",
          s != NULL && agentc_session_path(s) == NULL && agentc_session_id(s) != NULL);
    agentc_session_close(s);
}

/* ------------------------------------------------ mode/session queue cases */

/* Minimal mode config that never performs a network request: setup alone does
 * not start a run. */
static void fill_mode_cfg(AgcModeConfig *c, const char *dir, const char *id) {
    agentc_memset(c, 0, sizeof *c);
    c->provider = "anthropic";
    c->model = "claude-sonnet-4-5";
    c->api_key = "test";
    c->system = "sys";
    c->max_attempts = 1;
    c->session.cwd = ROOT;
    c->session.memory_only = false;
    c->session.dir = dir;
    c->session.id = id;
}

/* C4 (P8-D4 regression): /resume into a session with no `custom type:"queue"`
 * record must still REPLACE the live queue. Before the fix the reset lived
 * inside the items branch, so an empty target leaked the previous session's
 * prompts. */
static void test_resume_without_queue_replaces(void) {
    const char *dir = ROOT "/resumeq";
    (void)agentc_mkdir_parents(dir);
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);

    /* Session B: header only, no queue snapshot. */
    AgcModeConfig cb;
    fill_mode_cfg(&cb, dir, "emptyb01");
    AgcModeCtx c2;
    bool okb = agentc_mode_setup(&c2, &cb, &io, AGENTC_MODE_F_SESSION) == 0;
    char *bpath = (okb && c2.session) ? agentc_strdup(agentc_session_path(c2.session)) : NULL;
    agentc_mode_teardown(&c2);
    check("mode.resumeq.empty_b", bpath != NULL);

    /* Live session A with queued prompts. */
    AgcModeConfig ca;
    fill_mode_cfg(&ca, dir, "queueda1");
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &ca, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id = ok ? agentc_agent_actor(c.agent) : 0;
    if (ok) {
        (void)agentc_queue_add(id, "leak-me", 1, 0, 0, NULL);
        (void)agentc_queue_add(id, "leak-me-2", 2, 0, 0, NULL);
    }
    check("mode.resumeq.live_count", id != 0 && agentc_queue_count(id) == 2);

    /* Resume B: a switch replaces, never merges. */
    int rc = (ok && bpath) ? agentc_mode_resume_session(&c, bpath) : -1;
    AgcActorId id2 = ok ? agentc_agent_actor(c.agent) : 0;
    check("mode.resumeq.resume_ok", rc == 0);
    check("mode.resumeq.no_leak", id2 != 0 && agentc_queue_count(id2) == 0);
    agentc_mode_teardown(&c);
    agentc_free(bpath);
}

/* Queue `paused` never leaked/reset: agentc_queue_clear drops it, and a
 * snapshot's paused=true is re-applied on resume. */
static void test_queue_pause_restore(void) {
    const char *dir = ROOT "/pauseq";
    (void)agentc_mkdir_parents(dir);
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);

    AgcModeConfig cfg;
    fill_mode_cfg(&cfg, dir, "pausea01");
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id = ok ? agentc_agent_actor(c.agent) : 0;
    if (ok) {
        (void)agentc_queue_add(id, "x", 1, 0, 0, NULL);
        (void)agentc_queue_pause(id, true);
    }
    check("mode.pauseq.paused", id != 0 && agentc_queue_is_paused(id));
    /* Direction 1: a clear resets the pause. */
    check("mode.pauseq.clear_resets",
          id != 0 && agentc_queue_clear(id) == 0 && !agentc_queue_is_paused(id));
    /* Force the live queue unpaused through the public mutator so the resume
     * below can only observe the snapshot's `paused` value (the unfixed clear
     * leaves it set, which would mask the restore direction). This persists an
     * unpaused snapshot, overwritten by the paused=true seed just below. */
    if (ok)
        (void)agentc_queue_pause(id, false);

    /* Seed a snapshot with paused=true and one item, but leave the LIVE queue
     * unpaused so only the snapshot's flag can pause it on resume. */
    const char *path = (ok && c.session) ? agentc_session_path(c.session) : NULL;
    if (ok && path) {
        static const char snapshot[] =
            "{\"type\":\"custom\",\"custom_type\":\"queue\",\"data\":{"
            "\"paused\":true,\"items\":[{\"turn\":1,\"priority\":0,\"flags\":0,"
            "\"text\":\"y\"}]}}";
        check("mode.pauseq.snapshot_seed",
              agentc_session_append_raw(c.session, snapshot, agentc_strlen(snapshot)) == 0);
    }
    char *p1 = path ? agentc_strdup(path) : NULL;
    agentc_mode_teardown(&c);

    /* Direction 2: resume must replay the snapshot's paused=true. */
    AgcModeConfig cfg2;
    fill_mode_cfg(&cfg2, dir, "pauseb02");
    cfg2.session_spec = p1;
    AgcModeCtx c2;
    bool ok2 = p1 != NULL && agentc_mode_setup(&c2, &cfg2, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id2 = ok2 ? agentc_agent_actor(c2.agent) : 0;
    check("mode.pauseq.restored_paused", id2 != 0 && agentc_queue_is_paused(id2));
    check("mode.pauseq.restored_item", id2 != 0 && agentc_queue_count(id2) == 1);
    agentc_mode_teardown(&c2);
    agentc_free(p1);
}

/* SDK shutdown calls agentc_events_reset(), which wipes the hub but not any
 * cached subscription id. A second init/setup must re-subscribe, or queue
 * mutations stop persisting. */
static void test_events_reset_rearms(void) {
    const char *dir = ROOT "/rearm";
    (void)agentc_mkdir_parents(dir);
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);

    /* First cycle arms the persistence observers. */
    AgcModeConfig cfg;
    fill_mode_cfg(&cfg, dir, "rearm001");
    AgcModeCtx c;
    (void)agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION);
    agentc_mode_teardown(&c);

    /* Simulate SDK shutdown: the hub is cleared with the ids still cached. */
    agentc_events_reset();

    /* Second cycle must re-subscribe or this mutation is never persisted. */
    fill_mode_cfg(&cfg, dir, "rearm002");
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id = ok ? agentc_agent_actor(c.agent) : 0;
    if (ok)
        (void)agentc_queue_add(id, "after-reset", 1, 0, 0, NULL);
    const char *path = (ok && c.session) ? agentc_session_path(c.session) : NULL;
    char *p = path ? agentc_strdup(path) : NULL;
    agentc_mode_teardown(&c);

    size_t len = 0;
    char *text = p ? agentc_read_file_owned(p, &len) : NULL;
    check("mode.rearm.persisted",
          text && contains(text, "\"custom_type\":\"queue\"") && contains(text, "after-reset"));
    agentc_free(text);
    agentc_free(p);
}

/* `agentc_mode_agents_rebind(NULL, to)` must be a no-op: NULL is the documented
 * "unowned" binding, not a wildcard that rebinds every agent. */
static void test_rebind_null_noop(void) {
    (void)agentc_mkdir_parents(ROOT "/rebind");
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeConfig cfg;
    fill_mode_cfg(&cfg, ROOT "/rebind", "rebind01");
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcModeCtx other;
    agentc_memset(&other, 0, sizeof other);
    check("mode.rebind.owner_bound", ok && agentc_agent_owner(c.agent) == &c);
    /* Simulate an unowned agent: `from == NULL` must not match it. */
    if (ok)
        agentc_agent_set_owner(c.agent, NULL);
    if (ok)
        agentc_mode_agents_rebind(NULL, &other);
    check("mode.rebind.null_from_noop", ok && agentc_agent_owner(c.agent) == NULL);
    if (ok)
        agentc_agent_set_owner(c.agent, &c);
    /* `to == NULL` is likewise not a destination. */
    if (ok)
        agentc_mode_agents_rebind(&c, NULL);
    check("mode.rebind.null_to_noop", ok && agentc_agent_owner(c.agent) == &c);
    agentc_mode_teardown(&c);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_setenv("XDG_DATA_HOME", DATA);
    agentc_test_setenv("XDG_CONFIG_HOME", ROOT "/config");
    agentc_test_setenv("AGENTC_SESSION_DIR", NULL);
    agentc_rm_rf(ROOT);

    /* ------------------------------------------------------------ write */
    AgcTranscript src;
    agentc_transcript_init(&src);
    build_messages(&src);

    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = ROOT "/proj";
    o.id = "deadbeef";
    AgcSession *s = agentc_session_new(&o);
    char *path = agentc_strdup(agentc_session_path(s));
    check("path_present", path != NULL);
    check("path_pattern", contains(path, "/sessions/--tmp-agentc-session-test-proj--/") &&
                              ends_with(path, "_deadbeef.jsonl"));
    check("append_ok", agentc_session_append_message(s, &src.msgs[0]) == 0 &&
                           agentc_session_append_message(s, &src.msgs[1]) == 0 &&
                           agentc_session_append_message(s, &src.msgs[2]) == 0);
    agentc_session_close(s);

    size_t hlen = 0;
    char *header = agentc_read_file_owned(path, &hlen);
    check("header", header && contains(header, "\"type\":\"session\"") &&
                        contains(header, "\"version\":1") &&
                        contains(header, "\"id\":\"deadbeef\"") &&
                        contains(header, "\"cwd\":\"" ROOT "/proj\""));
    agentc_free(header);

    /* ------------------------------------------------ picker summary */
    {
        char prev[64];
        i64 ts = 0;
        check("summary_rc", agentc_session_summary(path, &ts, prev, sizeof prev) == 0);
        check("summary_ts", ts > 0);
        check("summary_preview", agentc_streq(prev, "hello world"));
    }

    /* ----------------------------------------------------------- replay */
    AgcSession *r = agentc_session_open(path);
    check("reopen", r != NULL && agentc_streq(agentc_session_id(r), "deadbeef"));
    AgcTranscript got;
    agentc_transcript_init(&got);
    check("load_rc", agentc_session_load_messages(r, &got) == 0);
    agentc_session_close(r);

    check("roundtrip_msgs", got.n == 3);
    bool roles = got.n == 3 && got.msgs[0].role == AGENTC_ROLE_USER &&
                 got.msgs[1].role == AGENTC_ROLE_ASSISTANT && got.msgs[2].role == AGENTC_ROLE_TOOL;
    check("roundtrip_roles", roles);
    check("roundtrip_text", got.n == 3 && agentc_streq(got.msgs[0].blocks[0].text, "hello world"));
    bool blocks = got.n == 3 && got.msgs[1].nblocks == 3 &&
                  got.msgs[1].blocks[0].type == AGENTC_BLK_THINK &&
                  agentc_streq(got.msgs[1].blocks[0].text, "thinking hard") &&
                  got.msgs[1].blocks[1].type == AGENTC_BLK_TEXT &&
                  agentc_streq(got.msgs[1].blocks[1].text, "answer") &&
                  got.msgs[1].blocks[2].type == AGENTC_BLK_TOOLCALL;
    check("roundtrip_blocks", blocks);
    check("roundtrip_args_raw",
          got.n == 3 && got.msgs[1].nblocks == 3 &&
              agentc_streq(got.msgs[1].blocks[2].tool_args, "{ \"path\" : \"a b.txt\" }"));
    check("roundtrip_tool_call", got.n == 3 && got.msgs[1].nblocks == 3 &&
                                     agentc_streq(got.msgs[1].blocks[2].tool_id, "call_1") &&
                                     agentc_streq(got.msgs[1].blocks[2].tool_name, "read"));
    check("roundtrip_tool_result", got.n == 3 &&
                                       agentc_streq(got.msgs[2].blocks[0].text, "file body\n") &&
                                       agentc_streq(got.msgs[2].blocks[0].tool_id, "call_1"));
    check("roundtrip_is_error", got.n == 3 && got.msgs[2].error != NULL);
    agentc_transcript_free(&got);
    agentc_transcript_free(&src);

    /* ------------------------------------------------------ memory only */
    AgcSessionOptions mo;
    agentc_memset(&mo, 0, sizeof mo);
    mo.cwd = ROOT "/proj";
    mo.memory_only = true;
    mo.dir = ROOT "/unused";
    AgcSession *mem = agentc_session_new(&mo);
    check("memory_only_path", agentc_session_path(mem) == NULL);
    AgcMsg *mm = agentc_transcript_push(&got, AGENTC_ROLE_USER);
    agentc_msg_add_text(mm, "x", 1);
    check("memory_only_append", agentc_session_append_message(mem, mm) == 0);
    check("memory_only_no_dir", !agentc_path_is_dir(ROOT "/unused"));
    agentc_session_close(mem);
    agentc_transcript_free(&got);

    /* ---------------------------------------------------------- listing */
    char list_dir[4096];
    check("list_dir_join", agentc_path_join(list_dir, sizeof list_dir, ROOT, "listdir"));
    const char *names[3] = {"1000_aaaaaaaa.jsonl", "2000_bbbbbbbb.jsonl", "1500_cccccccc.jsonl"};
    for (size_t i = 0; i < 3; i++) {
        char p[4200];
        if (!agentc_path_join(p, sizeof p, list_dir, names[i]))
            continue;
        AgcSession *ls = agentc_session_open(p);
        agentc_session_close(ls);
    }
    size_t n = 0;
    char **list = agentc_session_list(list_dir, &n, 0);
    check("list_count", n == 3);
    check("list_order", n == 3 && ends_with(list[0], "2000_bbbbbbbb.jsonl") &&
                            ends_with(list[1], "1500_cccccccc.jsonl") &&
                            ends_with(list[2], "1000_aaaaaaaa.jsonl"));
    agentc_sessions_free(list, n);
    list = agentc_session_list(list_dir, &n, 2);
    check("list_max", n == 2);
    agentc_sessions_free(list, n);

    char *latest = agentc_session_find_latest(list_dir, NULL);
    check("find_latest_explicit", latest && ends_with(latest, "2000_bbbbbbbb.jsonl"));
    agentc_free(latest);

    char *dflt_latest = agentc_session_find_latest(NULL, ROOT "/proj");
    check("find_latest_default", dflt_latest != NULL && ends_with(dflt_latest, "_deadbeef.jsonl"));
    agentc_free(dflt_latest);

    /* An explicit --session-dir can hold several projects: --continue must pick
     * the newest session recorded for this cwd, not the globally newest, and
     * fall back to the globally newest when no session matches. */
    {
        static const char sel_old[] =
            "{\"type\":\"session\",\"version\":1,\"id\":\"aaaa0001\",\"cwd\":\"/work/alpha\"}\n";
        static const char sel_new[] =
            "{\"type\":\"session\",\"version\":1,\"id\":\"bbbb0002\",\"cwd\":\"/work/beta\"}\n";
        const char *p_old = ROOT "/cwdsel/1000_aaaa0001.jsonl";
        const char *p_new = ROOT "/cwdsel/2000_bbbb0002.jsonl";
        (void)agentc_mkdir_parents(p_old);
        check("session.cwdsel.write_old", write_raw(p_old, sel_old, agentc_strlen(sel_old)));
        check("session.cwdsel.write_new", write_raw(p_new, sel_new, agentc_strlen(sel_new)));
        char *sel = agentc_session_find_latest(ROOT "/cwdsel", "/work/alpha");
        check("session.cwdsel.match", sel && ends_with(sel, "1000_aaaa0001.jsonl"));
        agentc_free(sel);
        char *fallback = agentc_session_find_latest(ROOT "/cwdsel", "/work/gamma");
        check("session.cwdsel.fallback", fallback && ends_with(fallback, "2000_bbbb0002.jsonl"));
        agentc_free(fallback);

        /* a header line longer than 8 KiB must still resolve its cwd. The long
         * file is the oldest, so only the cwd filter (not the newest-first
         * fallback) can pick it. */
        AgcBuf hb = {0};
        agentc_buf_cstr(&hb, "{\"type\":\"session\",\"version\":1,\"id\":\"long0001\","
                             "\"cwd\":\"/work/longproj\",\"model\":\"");
        for (int i = 0; i < 9000; i++)
            agentc_buf_byte(&hb, 'm');
        agentc_buf_cstr(&hb, "\"}\n");
        const char *p_long = ROOT "/cwdsel/0500_long0001.jsonl";
        (void)agentc_mkdir_parents(p_long);
        check("session.longheader.write", write_raw(p_long, (const char *)hb.p, hb.len));
        agentc_buf_free(&hb);
        char *longsel = agentc_session_find_latest(ROOT "/cwdsel", "/work/longproj");
        check("session.longheader.match", longsel && ends_with(longsel, "0500_long0001.jsonl"));
        agentc_free(longsel);
    }

    /* AGENTC_SESSION_DIR overrides the default per-cwd directory */
    agentc_test_setenv("AGENTC_SESSION_DIR", ROOT "/envdir");
    AgcSessionOptions eo;
    agentc_memset(&eo, 0, sizeof eo);
    eo.cwd = ROOT "/proj";
    eo.id = "envtest1";
    AgcSession *es = agentc_session_new(&eo);
    check("env_dir_path", es && contains(agentc_session_path(es), ROOT "/envdir/"));
    agentc_session_close(es);
    char *env_latest = agentc_session_find_latest(NULL, ROOT "/proj");
    check("env_dir_latest", env_latest && ends_with(env_latest, "_envtest1.jsonl"));
    agentc_free(env_latest);
    agentc_test_setenv("AGENTC_SESSION_DIR", NULL);
    agentc_free(path);

    /* ------------------------------------------- version refusal */
    (void)agentc_mkdir_parents(ROOT);
    {
        static const char v2text[] =
            "{\"type\":\"session\",\"version\":2,\"id\":\"v2\",\"cwd\":\"/tmp\"}\n"
            "{\"type\":\"message\",\"role\":\"user\",\"ts\":1,\"content\":[{\"type\":\"text\","
            "\"text\":\"new\"}]}\n";
        const char *v2 = ROOT "/verdir/version2.jsonl";
        (void)agentc_mkdir_parents(v2);
        check("session.version.refuse.write", write_raw(v2, v2text, agentc_strlen(v2text)));
        AgcSession *vo = agentc_session_open(v2);
        check("session.version.refuse.open", vo == NULL);
        if (vo)
            agentc_session_close(vo);

        /* load_messages refuses too; open a v1 file, then swap in a v2 header. */
        static const char v1text[] =
            "{\"type\":\"session\",\"version\":1,\"id\":\"swap\",\"cwd\":\"/tmp\"}\n";
        const char *vfile = ROOT "/verdir/swap.jsonl";
        (void)agentc_mkdir_parents(vfile);
        check("session.version.refuse.swap_write", write_raw(vfile, v1text, agentc_strlen(v1text)));
        AgcSession *vs = agentc_session_open(vfile);
        check("session.version.refuse.swap_open", vs != NULL);
        check("session.version.refuse.swap_rewrite",
              write_raw(vfile, v2text, agentc_strlen(v2text)));
        AgcTranscript vtr;
        agentc_transcript_init(&vtr);
        check("session.version.refuse.load", agentc_session_load_messages(vs, &vtr) == -71);
        agentc_transcript_free(&vtr);
        agentc_session_close(vs);
    }

    /* --------------------------------------- degrade + ftruncate */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/degraded";
        o.id = "degrade1";
        AgcSession *g = agentc_session_new(&o);
        check("session.degrade.create", g != NULL && agentc_session_path(g) != NULL);
        const char *gpath = agentc_session_path(g);
        AgcTranscript gtr;
        agentc_transcript_init(&gtr);
        AgcMsg *gm = agentc_transcript_push(&gtr, AGENTC_ROLE_USER);
        agentc_msg_add_text(gm, "durable", 7);
        check("session.degrade.append", agentc_session_append_message(g, gm) == 0);

        size_t before_len = 0;
        char *before = agentc_read_file_owned(gpath, &before_len);
        check("session.degrade.read", before != NULL);
        const char *garbage = "{\"type\":\"message\",\"role\":\"user\",\"text\":\"unfinished";
        check("session.degrade.garbage", append_raw_file(gpath, garbage, agentc_strlen(garbage)));
        size_t dirty_len = 0;
        char *dirty = agentc_read_file_owned(gpath, &dirty_len);
        check("session.degrade.larger",
              dirty != NULL && dirty_len == before_len + agentc_strlen(garbage));
        agentc_free(dirty);

        agentc_session_test_fail_next_append(g);
        check("session.degrade.fail", agentc_session_append_message(g, gm) == -5);
        size_t rolled_len = 0;
        char *rolled = agentc_read_file_owned(gpath, &rolled_len);
        check("session.degrade.rolled_back", rolled != NULL && rolled_len == before_len &&
                                                 agentc_memeq(rolled, before, before_len));
        agentc_free(rolled);

        size_t sticky_len = 0;
        char *sticky = agentc_read_file_owned(gpath, &sticky_len);
        check("session.degrade.sticky", agentc_session_append_message(g, gm) == -5 &&
                                            sticky != NULL && sticky_len == before_len);
        agentc_free(sticky);
        agentc_free(before);
        agentc_session_close(g);
        agentc_transcript_free(&gtr);
    }

    /* open() truncate fails: the torn tail stays, so the repair newline is
     * appended after the real physical end. A later failed append must roll
     * back to that same end, not into the torn line. */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/truncdir";
        o.id = "trunc001";
        AgcSession *ws = agentc_session_new(&o);
        char *trpath = agentc_strdup(agentc_session_path(ws));
        AgcTranscript wtr;
        agentc_transcript_init(&wtr);
        AgcMsg *um = agentc_transcript_push(&wtr, AGENTC_ROLE_USER);
        agentc_msg_add_text(um, "kept", 4);
        check("session.trunc.append", agentc_session_append_message(ws, um) == 0);
        agentc_session_close(ws);

        const char *torn = "{\"type\":\"message\",\"role\":\"user\",\"text\":\"torn";
        check("session.trunc.torn_write", append_raw_file(trpath, torn, agentc_strlen(torn)));
        size_t torn_len = 0;
        char *with_torn = agentc_read_file_owned(trpath, &torn_len);
        check("session.trunc.present", with_torn && contains(with_torn, "torn"));
        agentc_free(with_torn);

        agentc_session_test_fail_next_truncate(true);
        AgcSession *ts = agentc_session_open(trpath);
        check("session.trunc.open", ts != NULL);
        AgcMsg *t2 = agentc_transcript_push(&wtr, AGENTC_ROLE_USER);
        agentc_msg_add_text(t2, "repaired", 8);
        check("session.trunc.repair", ts != NULL && agentc_session_append_message(ts, t2) == 0);
        size_t repaired_len = 0;
        char *repaired = agentc_read_file_owned(trpath, &repaired_len);
        check("session.trunc.repaired",
              repaired && contains(repaired, "torn") && contains(repaired, "repaired"));

        agentc_session_test_fail_next_append(ts);
        check("session.trunc.fail", ts != NULL && agentc_session_append_message(ts, t2) == -5);
        size_t rollback_len = 0;
        char *rollback = agentc_read_file_owned(trpath, &rollback_len);
        check("session.trunc.rollback_real_end",
              rollback && repaired && rollback_len == repaired_len &&
                  agentc_memeq(rollback, repaired, repaired_len));
        agentc_free(rollback);
        agentc_free(repaired);
        agentc_session_close(ts);
        agentc_transcript_free(&wtr);
        agentc_free(trpath);
    }

    /* ------------------------------------------------- torn tail */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/taildir";
        o.id = "tail0001";
        AgcSession *ts = agentc_session_new(&o);
        AgcTranscript ttr;
        agentc_transcript_init(&ttr);
        AgcMsg *tm = agentc_transcript_push(&ttr, AGENTC_ROLE_USER);
        agentc_msg_add_text(tm, "kept", 4);
        check("session.tail.append", agentc_session_append_message(ts, tm) == 0);
        char *tpath = agentc_strdup(agentc_session_path(ts));
        agentc_session_close(ts);

        const char *torn = "{\"type\":\"message\",\"role\":\"user\",\"text\":\"torn";
        check("session.tail.torn_write", append_raw_file(tpath, torn, agentc_strlen(torn)));
        size_t torn_len = 0;
        char *with_torn = agentc_read_file_owned(tpath, &torn_len);
        check("session.tail.present", with_torn && contains(with_torn, "torn"));
        agentc_free(with_torn);

        AgcSession *to = agentc_session_open(tpath);
        check("session.tail.open", to != NULL);
        AgcMsg *t2 = agentc_transcript_push(&ttr, AGENTC_ROLE_USER);
        agentc_msg_add_text(t2, "after", 5);
        check("session.tail.append_after",
              to != NULL && agentc_session_append_message(to, t2) == 0);
        agentc_session_close(to);

        size_t final_len = 0;
        char *final = agentc_read_file_owned(tpath, &final_len);
        check("session.tail.truncated", final != NULL && !contains(final, "torn") &&
                                            final_len > 0 && final[final_len - 1] == '\n');
        agentc_free(final);

        AgcSession *lr = agentc_session_open(tpath);
        AgcTranscript load;
        agentc_transcript_init(&load);
        check("session.tail.load", lr != NULL && agentc_session_load_messages(lr, &load) == 0);
        check("session.tail.roundtrip", load.n == 2 &&
                                            agentc_streq(load.msgs[0].blocks[0].text, "kept") &&
                                            agentc_streq(load.msgs[1].blocks[0].text, "after"));
        agentc_transcript_free(&load);
        agentc_transcript_free(&ttr);
        agentc_session_close(lr);
        agentc_free(tpath);
    }

    /* ------------------------------------- arguments encoding */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/argsdir";
        o.id = "args0001";
        AgcSession *as = agentc_session_new(&o);
        char *apath = agentc_strdup(agentc_session_path(as));
        AgcTranscript atr;
        agentc_transcript_init(&atr);
        AgcMsg *am = agentc_transcript_push(&atr, AGENTC_ROLE_ASSISTANT);
        agentc_msg_add_tool_call(am, "call_a", "read");
        const char *raw = "{ \"path\" : \"a b.txt\" }";
        agentc_msg_tool_args_append(am, raw, agentc_strlen(raw));
        check("session.args.string.append", agentc_session_append_message(as, am) == 0);
        /* a matching result keeps the call through load reconciliation */
        AgcMsg *ares = agentc_transcript_push(&atr, AGENTC_ROLE_TOOL);
        agentc_msg_add_tool_result(ares, "call_a", "read", "ok");
        check("session.args.string.append_result", agentc_session_append_message(as, ares) == 0);
        agentc_session_close(as);

        size_t alen = 0;
        char *atext = agentc_read_file_owned(apath, &alen);
        check("session.args.string.shape",
              atext != NULL &&
                  contains(atext, "\"arguments\":\"{ \\\"path\\\" : \\\"a b.txt\\\" }\""));
        agentc_free(atext);

        AgcSession *ar = agentc_session_open(apath);
        AgcTranscript back;
        agentc_transcript_init(&back);
        check("session.args.string.load",
              ar != NULL && agentc_session_load_messages(ar, &back) == 0);
        check("session.args.string.roundtrip",
              back.n == 2 && back.msgs[0].role == AGENTC_ROLE_ASSISTANT &&
                  back.msgs[0].nblocks == 1 && back.msgs[0].blocks[0].type == AGENTC_BLK_TOOLCALL &&
                  agentc_streq(back.msgs[0].blocks[0].tool_args, raw) &&
                  back.msgs[1].role == AGENTC_ROLE_TOOL);
        agentc_transcript_free(&back);
        agentc_transcript_free(&atr);
        agentc_session_close(ar);
        agentc_free(apath);
    }

    /* torn turns are reconciled on load: an assistant tool_call with no result
     * and a tool result with no matching call are both dropped (crash / older
     * agents), so a replay never sends an unpaired tool block. */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/recdir";
        o.id = "rec00001";
        AgcSession *ws = agentc_session_new(&o);
        char *rpath = agentc_strdup(agentc_session_path(ws));
        AgcTranscript wtr;
        agentc_transcript_init(&wtr);
        AgcMsg *um = agentc_transcript_push(&wtr, AGENTC_ROLE_USER);
        agentc_msg_add_text(um, "hi", 2);
        (void)agentc_session_append_message(ws, um);
        AgcMsg *am = agentc_transcript_push(&wtr, AGENTC_ROLE_ASSISTANT);
        agentc_msg_add_tool_call(am, "c1", "read");
        agentc_msg_tool_args_append(am, "{}", 2);
        (void)agentc_session_append_message(ws, am);
        AgcMsg *tm = agentc_transcript_push(&wtr, AGENTC_ROLE_TOOL);
        agentc_msg_add_tool_result(tm, "c2", "read", "orphan");
        (void)agentc_session_append_message(ws, tm);
        agentc_session_close(ws);
        agentc_transcript_free(&wtr);

        AgcSession *rr = agentc_session_open(rpath);
        AgcTranscript rtr;
        agentc_transcript_init(&rtr);
        (void)agentc_session_load_messages(rr, &rtr);
        check("session.reconcile.orphans", rtr.n == 1 && rtr.msgs[0].role == AGENTC_ROLE_USER);
        agentc_transcript_free(&rtr);
        agentc_session_close(rr);
        agentc_free(rpath);
    }

    /* a multi-call assistant keeps every matching result: the matcher must
     * compare against the assistant, not the previous result slot */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/multidir";
        o.id = "multi001";
        AgcSession *ws = agentc_session_new(&o);
        char *mpath = agentc_strdup(agentc_session_path(ws));
        AgcTranscript wtr;
        agentc_transcript_init(&wtr);
        AgcMsg *am = agentc_transcript_push(&wtr, AGENTC_ROLE_ASSISTANT);
        agentc_msg_add_tool_call(am, "c0", "read");
        agentc_msg_add_tool_call(am, "c1", "read");
        (void)agentc_session_append_message(ws, am);
        const char *ids[2] = {"c0", "c1"};
        for (size_t k = 0; k < 2; k++) {
            AgcMsg *tm = agentc_transcript_push(&wtr, AGENTC_ROLE_TOOL);
            agentc_msg_add_tool_result(tm, ids[k], "read", "ok");
            (void)agentc_session_append_message(ws, tm);
        }
        agentc_session_close(ws);
        agentc_transcript_free(&wtr);

        AgcSession *rr = agentc_session_open(mpath);
        AgcTranscript rtr;
        agentc_transcript_init(&rtr);
        (void)agentc_session_load_messages(rr, &rtr);
        check("session.reconcile.multicall",
              rtr.n == 3 && rtr.msgs[0].nblocks == 2 && rtr.msgs[1].nblocks == 1 &&
                  rtr.msgs[2].nblocks == 1 && agentc_streq(rtr.msgs[1].blocks[0].tool_id, "c0") &&
                  agentc_streq(rtr.msgs[2].blocks[0].tool_id, "c1"));
        agentc_transcript_free(&rtr);
        agentc_session_close(rr);
        agentc_free(mpath);
    }

    /* malformed lines are skipped with one warning */
    {
        const char *mpath = ROOT "/malformed.jsonl";
        static const char malformed[] =
            "{\"type\":\"session\",\"version\":1,\"id\":\"malformed\",\"cwd\":\"/tmp\"}\n"
            "{\"oops\"\n"
            "not json at all\n"
            "{\"type\":\"message\",\"role\":\"user\",\"ts\":1,\"content\":[{\"type\":\"text\","
            "\"text\":\"survivor\"}]}\n";
        check("session.malformed.write", write_raw(mpath, malformed, agentc_strlen(malformed)));
        AgcSession *ms = agentc_session_open(mpath);
        AgcTranscript mtr;
        agentc_transcript_init(&mtr);
        check("session.malformed.load", ms != NULL && agentc_session_load_messages(ms, &mtr) == 0);
        check("session.malformed.skipped",
              mtr.n == 1 && agentc_streq(mtr.msgs[0].blocks[0].text, "survivor"));
        agentc_transcript_free(&mtr);
        agentc_session_close(ms);
    }

    /* a complete v1 header with no trailing newline is itself a torn tail: the
     * open truncates it and must rewrite a valid header instead of leaving the
     * file headerless. */
    {
        const char *hp = ROOT "/headerless.jsonl";
        static const char bare_header[] =
            "{\"type\":\"session\",\"version\":1,\"id\":\"nonl0001\",\"cwd\":\"/tmp/nonl\"}";
        (void)agentc_mkdir_parents(hp);
        check("session.header.no_newline.write",
              write_raw(hp, bare_header, agentc_strlen(bare_header)));
        AgcSession *hs = agentc_session_open(hp);
        check("session.header.no_newline.open", hs != NULL);
        agentc_session_close(hs);

        size_t flen = 0;
        char *fixed = agentc_read_file_owned(hp, &flen);
        check("session.header.rewritten", fixed != NULL && flen > 0 &&
                                              contains(fixed, "\"type\":\"session\"") &&
                                              fixed[flen - 1] == '\n');
        size_t lines = 0;
        for (size_t i = 0; i < flen; i++)
            if (fixed[i] == '\n')
                lines++;
        check("session.header.single", lines == 1);
        agentc_free(fixed);

        AgcSession *hr = agentc_session_open(hp);
        AgcTranscript htr;
        agentc_transcript_init(&htr);
        check("session.header.load",
              hr != NULL && agentc_session_load_messages(hr, &htr) == 0 && htr.n == 0);
        agentc_transcript_free(&htr);
        agentc_session_close(hr);
    }

    /* append_raw writes the record and its newline as one buffer, so the file
     * is always terminated even though the rollback offset can only undo a
     * whole write. */
    {
        AgcSessionOptions o;
        agentc_memset(&o, 0, sizeof o);
        o.cwd = ROOT "/proj";
        o.dir = ROOT "/rawdir";
        o.id = "raw00001";
        AgcSession *rs = agentc_session_new(&o);
        AgcTranscript rtr;
        agentc_transcript_init(&rtr);
        AgcMsg *rm = agentc_transcript_push(&rtr, AGENTC_ROLE_USER);
        agentc_msg_add_text(rm, "seed", 4);
        check("session.raw.seed", agentc_session_append_message(rs, rm) == 0);
        static const char rawrec[] = "{\"type\":\"message\",\"role\":\"user\",\"ts\":1,"
                                     "\"content\":[{\"type\":\"text\",\"text\":\"raw\"}]}";
        check("session.raw.append",
              agentc_session_append_raw(rs, rawrec, agentc_strlen(rawrec)) == 0);
        char *rpath = agentc_strdup(agentc_session_path(rs));
        agentc_session_close(rs);

        size_t rlen = 0;
        char *rtext = agentc_read_file_owned(rpath, &rlen);
        check("session.raw.terminated", rtext != NULL && rlen > 0 && rtext[rlen - 1] == '\n' &&
                                            contains(rtext, "\"text\":\"raw\""));
        agentc_free(rtext);

        AgcSession *rr = agentc_session_open(rpath);
        AgcTranscript rload;
        agentc_transcript_init(&rload);
        check("session.raw.replay",
              rr != NULL && agentc_session_load_messages(rr, &rload) == 0 && rload.n == 2);
        agentc_transcript_free(&rload);
        agentc_session_close(rr);
        agentc_transcript_free(&rtr);
        agentc_free(rpath);
    }

    agentc_rm_rf(ROOT);
    test_fork_tree();
    test_custom_all();
    test_degrade_memory_only();
    test_resume_without_queue_replaces();
    test_queue_pause_restore();
    test_events_reset_rearms();
    test_rebind_null_noop();
    agentc_rm_rf(ROOT);
    agentc_test_clearenv();
    return fails;
}
