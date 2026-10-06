/* dyn_test.c — dynamic extension loading (golden).
 *
 * A fake AgcExtDynLoader replaces the platform loader, so the whole scan →
 * open → adopt → register → load → unload/shutdown lifecycle runs against
 * fixture entries compiled into this binary. The filesystem side is real:
 * <XDG_CONFIG_HOME>/agentc/extensions/ holds one file per fixture plus
 * filtering and cap cases. Covered here:
 *   - scan suffix case-insensitivity, stem validation, no recursion, sort
 *     order and the 32-candidate cap with one overflow log;
 *   - agentc_ext_dyn_path_ok modes (regular/missing/dir and POSIX modes);
 *   - injected loader tables with a missing slot are refused;
 *   - duplicate skip before open, open/sym/ABI/name failures isolated,
 *     stem ⇔ out.name, entry rollback + close;
 *   - adopt owner rollback;
 *   - load order plus extensions.disabled;
 *   - unload close exactly once when idle;
 *   - live-job deferral until the last job unlinks (stop(UNLOAD) first);
 *   - deferred close from a defer callback (pump epilogue);
 *   - a status provider that unloads its own library mid-snapshot: no close
 *     inside the provider, close at the pump epilogue;
 *   - agentc_ext_shutdown close-when-idle vs keep-mapped when busy;
 *   - no use-after-close (fake poison) and the allocator baseline.
 *
 * The Windows run asserts the same golden output: the POSIX-only permission
 * cases are normalized through os_platform(), and no path or errno text with a
 * platform-specific value is printed.
 */
#include "agentc.h"
#include "plat.h"
#include "config.h"
#include "status.h"
#include "ext.h"
#include "ext/dynlib_int.h"
#include "core/tools/jobs.h"

void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);

#define DYN_ROOT       "/tmp/agentc-dyn-test"
#define DYN_XDG        DYN_ROOT "/xdg"
#define DYN_HOME       DYN_XDG "/agentc"
#define DYN_DIR        DYN_HOME "/extensions"
#define DYN_PERM_FILE  DYN_ROOT "/permcheck.fx"
#define DYN_PERM_DIR   DYN_ROOT "/permdir"
#define DYN_PERM_CHILD DYN_PERM_DIR "/world.fx"
/* Regression: a single-fixture config tree, so the self-unloading
 * status provider is the only status provider live during the snapshot. */
#define DYN_SU_XDG     DYN_ROOT "/xdg-statusunload"
#define DYN_SU_DIR     DYN_SU_XDG "/agentc/extensions"

#define CHECK_BUF 512

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool cstr_eq(const char *a, const char *b) {
    return a && b && agentc_streq(a, b);
}

/* ------------------------------------------------------------ file helpers */

static void write_stub(const char *dir, const char *name) {
    char path[CHECK_BUF];
    if (!agentc_path_join(path, sizeof path, dir, name)) return;
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (fd < 0) return;
    (void)os_write(fd, "fx", 2);
    (void)os_close(fd);
}

static void mkdir_one(const char *path) {
    (void)os_mkdir(path, 0755);
}

/* POSIX-only: chmod through /bin/sh (the test cannot call chmod(2)). The
 * parent PATH is preserved (a Nix host keeps chmod outside /usr/bin). */
static void sh_run(const char *script, const char *arg0, const char *arg1) {
    char path_env[4096];
    const char *parent_path = os_getenv("PATH");
    agentc_snprintf(path_env, sizeof path_env, "PATH=%s",
                    parent_path ? parent_path : "/usr/bin:/bin");
    char *envp[] = { path_env, NULL };
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)script,
                     (char *)arg0, (char *)arg1, NULL };
    int pid = os_spawn(argv, envp, NULL, -1, -1, -1, -1);
    if (pid > 0) (void)os_wait(pid, false);
}

static void sh_chmod(const char *mode, const char *path) {
    if (agentc_streq(os_platform(), "windows")) return;
    /* sh -c 'chmod "$1" "$0"' mode path -> chmod mode path */
    sh_run("chmod \"$1\" \"$0\"", path, mode);
}

static void sh_chmod_recursive_go_w(const char *path) {
    if (agentc_streq(os_platform(), "windows")) return;
    sh_run("chmod -R go-w \"$0\"", path, NULL);
}

/* --------------------------------------------------------------- fixtures */

enum {
    FIX_A1, FIX_A2, FIX_A3, FIX_BADABI, FIX_CASE, FIX_DISABLED, FIX_DUP,
    FIX_FAILINIT, FIX_HOOK, FIX_MISMATCH, FIX_NEVERUSED, FIX_NOSYM, FIX_ROLLBACK,
    FIX_SELFUNLOAD, FIX_STATUS, FIX_STATUSUNLOAD, FIX_SYNC, FIX_COUNT
};

typedef struct {
    const char *stem;
    int (*entry)(const AgcExtHost *host, AgcExt *out);
} Fixture;

typedef struct FakeHandle {
    int fixture;
    bool closed;
    struct FakeHandle *next;
} FakeHandle;

static FakeHandle *g_fake_handles;
static int g_fake_opens[FIX_COUNT];
static int g_fake_closes[FIX_COUNT];
static int g_fake_open_missing;
static int g_fake_poison;

static int fake_closes(int fixture) { return g_fake_closes[fixture]; }
static int fake_opens(int fixture) { return g_fake_opens[fixture]; }

/* ---------------------------------------------------------- fixture entries */

static const char *g_load_order[32];
static int g_load_n;

static void note_load(const char *name) {
    if (g_load_n < (int)(sizeof g_load_order / sizeof g_load_order[0]))
        g_load_order[g_load_n++] = name;
}

static void fill_ext(AgcExt *out, const char *name, const char *version, int32_t order,
                     int (*init)(const AgcExtHost *), void (*shutdown)(void)) {
    agentc_memset(out, 0, sizeof *out);
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = name;
    out->version = version;
    out->order = order;
    out->init = init;
    out->shutdown = shutdown;
}

/* --- sync tool --- */

static int sync_run(const AgcExtHost *host, const AgcExtTool *self,
                    const AgcExtToolCall *call, void *out, bool *is_error) {
    (void)self;
    (void)call;
    if (is_error) *is_error = false;
    host->out_write(out, "dyn-sync-ok", 11);
    return 0;
}

static const AgcExtTool sync_tool = {
    .struct_size = sizeof(AgcExtTool),
    .name = "dyn.sync",
    .label = "dyn sync",
    .description = "dynamic fixture tool",
    .parameters_json = "{}",
    .run = sync_run,
};

static int sync_init(const AgcExtHost *host) {
    note_load("sync");
    host->add_tool(&sync_tool);
    return 0;
}

static int sync_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "sync", "1.0", -5, sync_init, NULL);
    return 0;
}

/* --- observe hook (order 10, after status) --- */

static int g_hook_hits;

static int hook_fn(void *ud, const char *point, const char *payload, char **result) {
    (void)ud;
    (void)point;
    (void)payload;
    if (result) *result = NULL;
    g_hook_hits++;
    return 0;
}

static int hook_init(const AgcExtHost *host) {
    note_load("hook");
    (void)host->on("agent_start", AGENTC_HOOK_OBSERVE, 3, hook_fn, NULL);
    return 0;
}

static int hook_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "hook", "1.0", 10, hook_init, NULL);
    return 0;
}

/* --- status provider (order 5) --- */

static size_t status_provider(void *ud, AgcExtStatusSegment *out, size_t max,
                              char *arena, size_t arena_cap) {
    (void)ud;
    (void)arena;
    (void)arena_cap;
    if (max < 1) return 0;
    out[0].struct_size = sizeof(AgcExtStatusSegment);
    out[0].slot = 0;
    out[0].priority = 5;
    out[0].style = 0;
    out[0].text = "dyn-status";
    return 1;
}

static int status_init(const AgcExtHost *host) {
    note_load("status");
    host->add_status(status_provider, NULL);
    return 0;
}

static int status_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "status", "1.0", 5, status_init, NULL);
    return 0;
}

/* --- statusunload: a status provider that unloads its own library mid-call --- */

static int g_su_provider_calls;
static int g_su_closes_in_provider;
static int g_su_unloaded_in_provider;

static size_t su_status_provider(void *ud, AgcExtStatusSegment *out, size_t max,
                                 char *arena, size_t arena_cap) {
    (void)ud;
    (void)arena;
    (void)arena_cap;
    g_su_provider_calls++;
    agentc_ext_unload("statusunload");
    g_su_unloaded_in_provider = !agentc_ext_is_loaded("statusunload");
    g_su_closes_in_provider = fake_closes(FIX_STATUSUNLOAD);
    if (max < 1) return 0;
    out[0].struct_size = sizeof(AgcExtStatusSegment);
    out[0].slot = 0;
    out[0].priority = 5;
    out[0].style = 0;
    out[0].text = "su-seg";
    return 1;
}

static int statusunload_init(const AgcExtHost *host) {
    note_load("statusunload");
    host->add_status(su_status_provider, NULL);
    return 0;
}

static int statusunload_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "statusunload", "1.0", 0, statusunload_init, NULL);
    return 0;
}

/* --- async trio (a1 unload-in-step, a2 shutdown-in-step, a3 stays running) --- */

typedef struct {
    int which;
    int deferred;
    int token_ok;
} DynAsyncState;

static int g_a1_unload_in_step;
static int g_a1_unloaded;
static int g_a1_closes_during_unload;
static int g_a1_stop_count;
static int g_a1_stop_reason;
static int g_a2_shutdown_in_step;
static int g_a2_shutdown_called;
static int g_a2_closes_at_shutdown;
static int g_a2_stop_count;
static int g_a2_stop_reason;
static int g_a3_stop_count;
static int g_async_bad;

static void a2_shutdown_cb(void *ud) {
    (void)ud;
    g_a2_shutdown_called = 1;
    agentc_ext_shutdown();
    g_a2_closes_at_shutdown = fake_closes(FIX_A2);
}

static int async_start(const AgcExtHost *host, const AgcExtTool *self,
                       const AgcExtToolCall *call, void *out, bool *is_error,
                       void **state) {
    (void)out;
    DynAsyncState *st = host->alloc(sizeof *st);
    st->which = *(const int *)self->ud;
    st->deferred = 0;
    st->token_ok = call && call->signal_token && call->signal_token[0];
    *state = st;
    if (is_error) *is_error = false;
    return 0;
}

static int async_step(const AgcExtHost *host, const AgcExtTool *self,
                      const AgcExtToolCall *call, void *state, void *out,
                      bool *is_error) {
    (void)self;
    (void)call;
    DynAsyncState *st = state;
    if (!st || !st->token_ok) {
        g_async_bad = 1;
        return -22;
    }
    if (is_error) *is_error = false;
    if (st->which == 1 && g_a1_unload_in_step && !g_a1_unloaded) {
        g_a1_unloaded = 1;
        agentc_ext_unload("a1");
        g_a1_closes_during_unload = fake_closes(FIX_A1);
        return 1;
    }
    if (st->which == 2 && g_a2_shutdown_in_step && !st->deferred) {
        st->deferred = 1;
        host->defer(host, a2_shutdown_cb, NULL);
        return 0;   /* the adapter pumps after a running step (D-A9) */
    }
    if (st->which == 3) {
        host->out_write(out, "a3-step", 7);
        return 0;   /* stays running; only teardown stops it */
    }
    return 1;
}

static void async_stop(const AgcExtHost *host, const AgcExtTool *self, void *state,
                       int reason) {
    int which = *(const int *)self->ud;
    if (which == 1) {
        g_a1_stop_count++;
        g_a1_stop_reason = reason;
    } else if (which == 2) {
        g_a2_stop_count++;
        g_a2_stop_reason = reason;
    } else {
        g_a3_stop_count++;
    }
    host->free(state);
}

static int g_ud_which_a1 = 1, g_ud_which_a2 = 2, g_ud_which_a3 = 3;

static const AgcExtTool async_tool_a1 = {
    .struct_size = sizeof(AgcExtTool), .name = "dyn.a1", .label = "dyn a1",
    .description = "", .parameters_json = "{}", .ud = &g_ud_which_a1,
    .timeout_ms = 60000, .start = async_start, .step = async_step, .stop = async_stop,
};
static const AgcExtTool async_tool_a2 = {
    .struct_size = sizeof(AgcExtTool), .name = "dyn.a2", .label = "dyn a2",
    .description = "", .parameters_json = "{}", .ud = &g_ud_which_a2,
    .timeout_ms = 60000, .start = async_start, .step = async_step, .stop = async_stop,
};
static const AgcExtTool async_tool_a3 = {
    .struct_size = sizeof(AgcExtTool), .name = "dyn.a3", .label = "dyn a3",
    .description = "", .parameters_json = "{}", .ud = &g_ud_which_a3,
    .timeout_ms = 60000, .start = async_start, .step = async_step, .stop = async_stop,
};

static int a1_init(const AgcExtHost *host) {
    note_load("a1");
    host->add_tool(&async_tool_a1);
    return 0;
}
static int a2_init(const AgcExtHost *host) {
    note_load("a2");
    host->add_tool(&async_tool_a2);
    return 0;
}
static int a3_init(const AgcExtHost *host) {
    note_load("a3");
    host->add_tool(&async_tool_a3);
    return 0;
}

static int a1_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "a1", "1.0", 0, a1_init, NULL);
    return 0;
}
static int a2_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "a2", "1.0", 0, a2_init, NULL);
    return 0;
}
static int a3_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "a3", "1.0", 0, a3_init, NULL);
    return 0;
}

/* --- case: deferred close from inside a defer callback --- */

static int g_case_schedule;
static int g_case_closes_in_cb;

static void case_defer_cb(void *ud) {
    (void)ud;
    agentc_ext_unload("case");
    g_case_closes_in_cb = fake_closes(FIX_CASE);
}

static int case_init(const AgcExtHost *host) {
    note_load("case");
    if (g_case_schedule) host->defer(host, case_defer_cb, NULL);
    return 0;
}

static int case_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "case", "1.0", 0, case_init, NULL);
    return 0;
}

/* --- failing init + shutdown --- */

static int g_failinit_inits;
static int g_failinit_shutdowns;

static int failinit_init(const AgcExtHost *host) {
    (void)host;
    g_failinit_inits++;
    return -5;
}

static void failinit_shutdown(void) { g_failinit_shutdowns++; }

static int failinit_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "failinit", "1.0", 0, failinit_init, failinit_shutdown);
    return 0;
}

/* --- bad ABI (register refused) --- */

static int badabi_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "badabi", "1.0", 0, NULL, NULL);
    out->abi_version = AGENTC_EXT_ABI + 1;
    return 0;
}

/* --- name mismatch (register refused) --- */

static int mismatch_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "not_mismatch", "1.0", 0, NULL, NULL);
    return 0;
}

/* --- entry rollback: registers a tool, then fails validation --- */

static const AgcExtTool rollback_tool = {
    .struct_size = sizeof(AgcExtTool), .name = "dyn.rollback", .label = "rollback",
    .description = "", .parameters_json = "{}", .run = sync_run,
};

static int rollback_entry(const AgcExtHost *host, AgcExt *out) {
    host->add_tool(&rollback_tool);
    fill_ext(out, "rollback", "1.0", 0, NULL, NULL);
    out->abi_version = AGENTC_EXT_ABI + 1;   /* refused after the add */
    return 0;
}

/* --- selfunload: init re-entrantly unloads its own record --- */

static int selfunload_init(const AgcExtHost *host) {
    (void)host;
    agentc_ext_unload("selfunload");   /* removes the record while init runs */
    return 0;
}

static int selfunload_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "selfunload", "1.0", 0, selfunload_init, NULL);
    return 0;
}

/* --- never used: no init, no contributions --- */

static int neverused_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "neverused", "1.0", 0, NULL, NULL);
    return 0;
}
/* --- disabled fixture (config-excluded in its own phase) --- */

static int g_disabled_init_calls;

static int disabled_init(const AgcExtHost *host) {
    (void)host;
    g_disabled_init_calls++;
    note_load("disabled_fx");
    return 0;
}

static int disabled_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    fill_ext(out, "disabled_fx", "1.0", 0, disabled_init, NULL);
    return 0;
}

/* --- dup: a linked extension of this name is registered first --- */

static int g_dup_entry_calls;
static int g_dup_static_loaded;

static int dup_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    g_dup_entry_calls++;
    fill_ext(out, "dup", "1.0", 0, NULL, NULL);
    return 0;
}

static int dup_static_init(const AgcExtHost *host) {
    (void)host;
    g_dup_static_loaded = 1;
    return 0;
}

static const AgcExt dup_static = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "dup",
    .version = "linked",
    .init = dup_static_init,
};

static const Fixture g_fixtures[] = {
    [FIX_A1] = { "a1", a1_entry },
    [FIX_A2] = { "a2", a2_entry },
    [FIX_A3] = { "a3", a3_entry },
    [FIX_BADABI] = { "badabi", badabi_entry },
    [FIX_CASE] = { "case", case_entry },
    [FIX_DISABLED] = { "disabled_fx", disabled_entry },
    [FIX_DUP] = { "dup", dup_entry },
    [FIX_FAILINIT] = { "failinit", failinit_entry },
    [FIX_HOOK] = { "hook", hook_entry },
    [FIX_MISMATCH] = { "mismatch", mismatch_entry },
    [FIX_NEVERUSED] = { "neverused", neverused_entry },
    [FIX_NOSYM] = { "nosym", NULL },
    [FIX_ROLLBACK] = { "rollback", rollback_entry },
    [FIX_SELFUNLOAD] = { "selfunload", selfunload_entry },
    [FIX_STATUS] = { "status", status_entry },
    [FIX_STATUSUNLOAD] = { "statusunload", statusunload_entry },
    [FIX_SYNC] = { "sync", sync_entry },
};

/* ------------------------------------------------------------- fake loader */

static int fixture_from_path(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    size_t n = agentc_strlen(base);
    if (n <= 3) return -1;   /* ".fx" or shorter */
    size_t stem_len = n - 3;
    if (stem_len > 64) return -1;
    for (size_t i = 0; i < sizeof g_fixtures / sizeof g_fixtures[0]; i++) {
        const char *stem = g_fixtures[i].stem;
        if (agentc_strlen(stem) != stem_len) continue;
        bool same = true;
        for (size_t k = 0; k < stem_len; k++) {
            char c = base[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != stem[k]) { same = false; break; }
        }
        if (same) return (int)i;
    }
    return -1;
}

static void fake_err(char *err, size_t errcap, const char *msg) {
    if (!err || errcap == 0) return;
    size_t n = agentc_strlen(msg);
    if (n >= errcap) n = errcap - 1;
    agentc_memcpy(err, msg, n);
    err[n] = 0;
}

static int fake_open(const char *path, void **handle, char *err, size_t errcap) {
    if (err && errcap) err[0] = 0;
    int fixture = fixture_from_path(path);
    if (fixture < 0) {
        g_fake_open_missing++;
        fake_err(err, errcap, "fake: no such fixture");
        return -2;
    }
    FakeHandle *h = agentc_alloc(sizeof *h);
    h->fixture = fixture;
    h->closed = false;
    h->next = g_fake_handles;
    g_fake_handles = h;
    g_fake_opens[fixture]++;
    if (handle) *handle = h;
    return 0;
}

static int fake_sym(void *handle, const char *name, void **out, char *err,
                    size_t errcap) {
    if (err && errcap) err[0] = 0;
    FakeHandle *h = handle;
    if (!h || h->closed) {
        g_fake_poison++;
        fake_err(err, errcap, "fake: handle is closed");
        return -5;
    }
    if (!agentc_streq(name, "agentc_ext_init") || !g_fixtures[h->fixture].entry) {
        fake_err(err, errcap, "fake: no such symbol");
        return -2;
    }
    if (out) *out = (void *)g_fixtures[h->fixture].entry;
    return 0;
}

static int fake_close(void *handle) {
    FakeHandle *h = handle;
    if (!h) return 0;
    if (h->closed) {
        g_fake_poison++;
        return -5;
    }
    h->closed = true;
    g_fake_closes[h->fixture]++;
    return 0;
}

static bool fake_available(void) { return true; }
static const char *fake_suffix(void) { return ".fx"; }

static const AgcExtDynLoader g_fake_loader = {
    .open = fake_open,
    .sym = fake_sym,
    .close = fake_close,
    .available = fake_available,
    .suffix = fake_suffix,
};

static void fake_reset_counts(void) {
    agentc_memset(g_fake_opens, 0, sizeof g_fake_opens);
    agentc_memset(g_fake_closes, 0, sizeof g_fake_closes);
    g_fake_open_missing = 0;
    g_fake_poison = 0;
}

/* Handles are freed only here: a closed handle is intentionally left in the
 * list so a second close/sym would trip the poison counter first. */
static void fake_free_handles(void) {
    FakeHandle *h = g_fake_handles;
    while (h) {
        FakeHandle *next = h->next;
        agentc_free(h);
        h = next;
    }
    g_fake_handles = NULL;
}

/* ------------------------------------------------------------ host helpers */

static const AgcTool *find_tool(const char *name, AgcTool *tools, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(tools[i].name, name)) return &tools[i];
    return NULL;
}

static size_t collect_tools(AgcTool *out, size_t max) {
    return agentc_ext_tools(out, max);
}

static int describe_state(const char *name) {
    size_t n = agentc_ext_describe(NULL, 0);
    AgcExtInfo *info = agentc_alloc((n ? n : 1) * sizeof *info);
    n = agentc_ext_describe(info, n);
    int state = -1;
    for (size_t i = 0; i < n; i++) {
        if (agentc_streq(info[i].name, name)) { state = info[i].state; break; }
    }
    agentc_free(info);
    return state;
}

static bool describe_dynamic(const char *name) {
    size_t n = agentc_ext_describe(NULL, 0);
    AgcExtInfo *info = agentc_alloc((n ? n : 1) * sizeof *info);
    n = agentc_ext_describe(info, n);
    bool dynamic = false;
    for (size_t i = 0; i < n; i++) {
        if (agentc_streq(info[i].name, name)) { dynamic = info[i].dynamic; break; }
    }
    agentc_free(info);
    return dynamic;
}

static int describe_count(const char *name) {
    size_t n = agentc_ext_describe(NULL, 0);
    AgcExtInfo *info = agentc_alloc((n ? n : 1) * sizeof *info);
    n = agentc_ext_describe(info, n);
    int count = 0;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(info[i].name, name)) count++;
    agentc_free(info);
    return count;
}

static bool status_has(const char *text) {
    AgcStatusValue segs[AGENTC_STATUS_MAX_SEGMENTS];
    size_t n = agentc_status_snapshot(segs, AGENTC_STATUS_MAX_SEGMENTS);
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, text)) return true;
    return false;
}

typedef struct {
    AgcBuf out;
    bool is_error;
    bool done;
} RunCtx;

static int run_cb(void *ud, int what, const AgcJob *job) {
    RunCtx *c = ud;
    if (what == AGENTC_JOB_STARTED) return 0;
    if (job->out.len) agentc_buf_push(&c->out, job->out.p, job->out.len);
    c->is_error = job->is_error;
    c->done = true;
    return 0;
}

static void run_tool_copy(const AgcTool *tool, RunCtx *c) {
    agentc_memset(c, 0, sizeof *c);
    if (!tool) return;
    AgcToolCall call = { "dyn-call", tool->name, "{}", NULL };
    (void)agentc_tool_jobs_run(&tool, NULL, &call, 1, run_cb, c);
}

/* The D-A9 pump the async adapter calls after a running step. */
static void test_pump(void *ud, int timeout_ms) {
    (void)ud;
    (void)timeout_ms;
    agentc_ext_pump();
}

/* ------------------------------------------------------- scan collection */

typedef struct {
    char names[64][72];
    size_t n;
} ScanLog;

static void scan_cb(void *ud, const char *stem, const char *path) {
    (void)path;
    ScanLog *s = ud;
    if (s->n < sizeof s->names / sizeof s->names[0]) {
        agentc_snprintf(s->names[s->n], sizeof s->names[0], "%s", stem);
        s->n++;
    }
}

static bool scan_has(const ScanLog *s, const char *stem) {
    for (size_t i = 0; i < s->n; i++)
        if (agentc_streq(s->names[i], stem)) return true;
    return false;
}

/* Every offered stem must satisfy the [a-z0-9_.:-]{1,64} rule. */
static bool scan_stems_valid(const ScanLog *s) {
    for (size_t i = 0; i < s->n; i++) {
        size_t len = agentc_strlen(s->names[i]);
        if (len == 0 || len > 64) return false;
        for (size_t k = 0; k < len; k++) {
            char c = s->names[i][k];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == ':' || c == '.' || c == '-';
            if (!ok) return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- main phases */

static void make_tree(void) {
    agentc_rm_rf(DYN_ROOT);
    char parent[CHECK_BUF];
    agentc_snprintf(parent, sizeof parent, "%s", DYN_DIR "/_");
    (void)agentc_mkdir_parents(parent);
    mkdir_one(DYN_DIR);
    char su_parent[CHECK_BUF];
    agentc_snprintf(su_parent, sizeof su_parent, "%s", DYN_SU_DIR "/_");
    (void)agentc_mkdir_parents(su_parent);
    mkdir_one(DYN_SU_DIR);
    mkdir_one(DYN_ROOT "/permdir");

    static const char *files[] = {
        "a1.fx", "a2.fx", "a3.fx", "badabi.fx", "case.FX", "disabled_fx.fx",
        "dup.fx", "failinit.fx", "hook.fx", "mismatch.fx", "neverused.fx",
        "nosym.fx", "orphan.fx", "rollback.fx", "selfunload.fx", "status.fx",
        "sync.fx",
        /* rejected by scan: wrong suffix, invalid stems, a directory, no
         * recursion into sub/ */
        "plain.txt", "UPPER.FX", "Bad name.fx",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
        write_stub(DYN_DIR, files[i]);
    mkdir_one(DYN_DIR "/dir.fx");   /* a directory with a library suffix */
    char longname[96];
    agentc_memset(longname, 'a', 65);
    agentc_memcpy(longname + 65, ".fx", 4);
    write_stub(DYN_DIR, longname);
    mkdir_one(DYN_DIR "/sub");
    write_stub(DYN_DIR "/sub", "inner.fx");
    for (int i = 0; i < 40; i++) {
        char name[32];
        agentc_snprintf(name, sizeof name, "cap%02d.fx", i);
        write_stub(DYN_DIR, name);
    }
    write_stub(DYN_ROOT, "permcheck.fx");
    write_stub(DYN_PERM_DIR, "world.fx");
    /* The self-unloading status provider lives alone in its own
     * config home, so its snapshot has exactly one provider. */
    write_stub(DYN_SU_DIR, "statusunload.fx");

    /* The ambient umask must not decide the policy checks: strip group/other
     * write from the tree, then re-add it where the test needs it. */
    sh_chmod_recursive_go_w(DYN_ROOT);
    sh_chmod("666", DYN_PERM_FILE);
    sh_chmod("777", DYN_PERM_DIR);
}

static void remove_cap_files(void) {
    for (int i = 0; i < 40; i++) {
        char name[32];
        char path[CHECK_BUF];
        agentc_snprintf(name, sizeof name, "cap%02d.fx", i);
        if (agentc_path_join(path, sizeof path, DYN_DIR, name)) (void)os_unlink(path);
    }
}

static void test_scan(void) {
    ScanLog s;
    agentc_memset(&s, 0, sizeof s);
    size_t n = agentc_ext_dyn_scan(scan_cb, &s);
    agentc_outf("scan_count=%llu\n", (unsigned long long)n);
    agentc_outf("scan_first=%s\n", s.n ? s.names[0] : "");
    agentc_outf("scan_last=%s\n", s.n ? s.names[s.n - 1] : "");
    check("scan_cap", n == 32 && s.n == 32);
    check("scan_first_a1", s.n > 0 && agentc_streq(s.names[0], "a1"));
    check("scan_last_cap27", s.n > 0 && agentc_streq(s.names[s.n - 1], "cap27"));

    /* Without the cap files the full filtered set is visible, so the suffix
     * case-insensitivity and the rejected names can be asserted directly. */
    remove_cap_files();
    ScanLog f;
    agentc_memset(&f, 0, sizeof f);
    size_t fn = agentc_ext_dyn_scan(scan_cb, &f);
    agentc_outf("scan_filtered=%llu\n", (unsigned long long)fn);
    check("scan_filtered_count", fn == 17 && f.n == 17);
    check("scan_suffix_case_insensitive", scan_has(&f, "case"));
    check("scan_no_invalid_stem", scan_stems_valid(&f) && !scan_has(&f, "UPPER") &&
                                      !scan_has(&f, "dir"));
    check("scan_no_recursion", !scan_has(&f, "inner"));
    check("scan_no_wrong_suffix", !scan_has(&f, "plain"));

    /* A missing extensions directory is not an error. */
    agentc_test_setenv("XDG_CONFIG_HOME", DYN_ROOT "/does-not-exist");
    ScanLog m;
    agentc_memset(&m, 0, sizeof m);
    check("scan_missing_dir", agentc_ext_dyn_scan(scan_cb, &m) == 0 && m.n == 0);

    /* A relative config home is refused: scanning from it would resolve against
     * the process CWD and load libraries from wherever agentc happens to run. */
    agentc_test_setenv("XDG_CONFIG_HOME", "relative/xdg");
    ScanLog rel;
    agentc_memset(&rel, 0, sizeof rel);
    check("scan_relative_home", agentc_ext_dyn_scan(scan_cb, &rel) == 0 && rel.n == 0);
    agentc_test_setenv("XDG_CONFIG_HOME", DYN_XDG);
}

static void test_path_ok(void) {
    bool win = agentc_streq(os_platform(), "windows");
    char regular[CHECK_BUF], missing[CHECK_BUF];
    if (!agentc_path_join(regular, sizeof regular, DYN_DIR, "sync.fx")) regular[0] = 0;
    if (!agentc_path_join(missing, sizeof missing, DYN_DIR, "gone.fx")) missing[0] = 0;
    check("path_ok_regular", agentc_ext_dyn_path_ok(regular));
    check("path_ok_missing", !agentc_ext_dyn_path_ok(missing));
    check("path_ok_directory", !agentc_ext_dyn_path_ok(DYN_DIR));
    /* Windows has no group/other mode bits, so its permission half is a no-op
     * (LoadLibraryExW's search flags carry the safety); POSIX rejects. */
    check("path_ok_file_perm",
          win ? agentc_ext_dyn_path_ok(DYN_PERM_FILE) : !agentc_ext_dyn_path_ok(DYN_PERM_FILE));
    check("path_ok_dir_perm",
          win ? agentc_ext_dyn_path_ok(DYN_PERM_CHILD) : !agentc_ext_dyn_path_ok(DYN_PERM_CHILD));
}

static void test_loader_reject(void) {
    const AgcExtDynLoader *before = agentc_ext_dyn_loader();
    /* Missing close+available+suffix, then missing close only: both refused. */
    static const AgcExtDynLoader open_sym_only = {
        .open = fake_open,
        .sym = fake_sym,
    };
    static const AgcExtDynLoader without_close = {
        .open = fake_open,
        .sym = fake_sym,
        .available = fake_available,
        .suffix = fake_suffix,
    };
    agentc_ext_dyn_set_loader(&open_sym_only);
    check("loader_reject_missing_slots", agentc_ext_dyn_loader() == before);
    agentc_ext_dyn_set_loader(&without_close);
    check("loader_reject_missing_close", agentc_ext_dyn_loader() == before);
    agentc_ext_dyn_set_loader(&g_fake_loader);
    check("loader_accept_complete", agentc_ext_dyn_loader() == &g_fake_loader);
}

static void test_register_and_lifecycle(void) {
    fake_reset_counts();
    g_load_n = 0;
    g_hook_hits = 0;
    g_a1_unload_in_step = 0;
    g_a1_unloaded = 0;
    g_a1_closes_during_unload = 0;
    g_a1_stop_count = 0;
    g_a1_stop_reason = -1;
    g_a2_shutdown_in_step = 0;
    g_a2_shutdown_called = 0;
    g_a2_closes_at_shutdown = -1;
    g_a2_stop_count = 0;
    g_a2_stop_reason = -1;
    g_a3_stop_count = 0;
    g_async_bad = 0;
    g_case_schedule = 1;
    g_case_closes_in_cb = -1;
    g_failinit_inits = 0;
    g_failinit_shutdowns = 0;
    g_disabled_init_calls = 0;
    g_dup_entry_calls = 0;
    g_dup_static_loaded = 0;

    /* A linked extension named dup makes the dynamic copy a pre-open skip. */
    agentc_ext_register(&dup_static);
    agentc_ext_register_dynamic();

    check("reg_dup_not_opened", fake_opens(FIX_DUP) == 0);
    check("reg_dup_entry_not_called", g_dup_entry_calls == 0);
    check("reg_badabi_closed", fake_closes(FIX_BADABI) == 1);
    check("reg_mismatch_closed", fake_closes(FIX_MISMATCH) == 1);
    check("reg_rollback_closed", fake_closes(FIX_ROLLBACK) == 1);
    check("reg_nosym_closed", fake_closes(FIX_NOSYM) == 1);
    check("reg_orphan_open_missing", g_fake_open_missing == 1);
    check("reg_rollback_tool_gone", collect_tools(NULL, 0) == 0);
    check("reg_pending_state",
          describe_state("sync") == AGENTC_EXT_STATE_PENDING &&
              describe_dynamic("sync"));
    check("reg_no_use_after_close", g_fake_poison == 0);

    agentc_ext_load_all();

    AgcBuf order = { 0 };
    for (int i = 0; i < g_load_n; i++) {
        if (i) agentc_buf_cstr(&order, ":");
        agentc_buf_cstr(&order, g_load_order[i]);
    }
    agentc_outf("load_order=%s\n", order.p ? (const char *)order.p : "");
    agentc_buf_free(&order);

    check("load_sync_first", g_load_n > 0 && agentc_streq(g_load_order[0], "sync"));
    check("load_order_tracked", g_load_n == 8);
    check("load_dup_static", g_dup_static_loaded && describe_count("dup") == 1);
    check("load_all_present",
          agentc_ext_is_loaded("sync") && agentc_ext_is_loaded("hook") &&
              agentc_ext_is_loaded("status") && agentc_ext_is_loaded("a1") &&
              agentc_ext_is_loaded("neverused"));
    check("load_failinit_failed", describe_state("failinit") == AGENTC_EXT_STATE_FAILED);
    check("load_failinit_ran", g_failinit_inits == 1 && g_failinit_shutdowns == 1);
    check("load_failinit_closed", fake_closes(FIX_FAILINIT) == 1);
    check("load_status_before_hook", g_load_n == 8 &&
                                         agentc_streq(g_load_order[6], "status") &&
                                         agentc_streq(g_load_order[7], "hook"));
    check("load_selfunload_guard", describe_state("selfunload") == -1 &&
                                        fake_closes(FIX_SELFUNLOAD) == 0);

    AgcTool tools[16];
    size_t ntools = collect_tools(tools, 16);
    const AgcTool *sync_tool_ptr = find_tool("dyn.sync", tools, ntools);
    check("tools_async_trio", find_tool("dyn.a1", tools, ntools) &&
                                  find_tool("dyn.a2", tools, ntools) &&
                                  find_tool("dyn.a3", tools, ntools));
    check("tools_rollback_absent", !find_tool("dyn.rollback", tools, ntools));

    RunCtx rc;
    run_tool_copy(sync_tool_ptr, &rc);
    check("sync_run", rc.done && !rc.is_error && cstr_eq((const char *)rc.out.p,
                                                          "dyn-sync-ok"));
    agentc_buf_free(&rc.out);

    AgcExtResult er = agentc_ext_emit("agent_start", "{}");
    check("hook_emitted", g_hook_hits == 1);
    agentc_free(er.result_json);
    check("status_registered", status_has("dyn-status"));

    /* unload when idle: closes exactly once; a second unload is a no-op. */
    agentc_ext_unload("sync");
    check("unload_sync_closed", fake_closes(FIX_SYNC) == 1);
    check("unload_sync_gone", !agentc_ext_is_loaded("sync"));
    agentc_ext_unload("sync");
    check("unload_sync_twice_once", fake_closes(FIX_SYNC) == 1);

    /* live async job: the close is deferred until the job unlinks. */
    g_a1_unload_in_step = 1;
    const AgcTool *a1 = find_tool("dyn.a1", tools, ntools);
    run_tool_copy(a1, &rc);
    check("live_job_no_close_during_unload", g_a1_unloaded &&
                                                 g_a1_closes_during_unload == 0);
    check("live_job_closed_after_unlink", fake_closes(FIX_A1) == 1);
    check("live_job_stop_unload_once",
          g_a1_stop_count == 1 && g_a1_stop_reason == AGENTC_EXT_TOOL_UNLOAD);
    check("live_job_no_step_after_unload", !g_async_bad);
    agentc_buf_free(&rc.out);

    /* deferred close from a defer callback: only the pump epilogue closes. */
    agentc_ext_pump();
    check("callback_close_deferred", g_case_closes_in_cb == 0);
    check("callback_closed_in_pump_epilogue", fake_closes(FIX_CASE) == 1);
    check("callback_unloaded", !agentc_ext_is_loaded("case"));
    check("selfunload_closed_deferred", fake_closes(FIX_SELFUNLOAD) == 1);

    /* shutdown closes every remaining idle mapping. */
    agentc_ext_shutdown();
    check("shutdown_neverused_closed", fake_closes(FIX_NEVERUSED) == 1);
    check("shutdown_status_closed", fake_closes(FIX_STATUS) == 1);
    check("shutdown_hook_closed", fake_closes(FIX_HOOK) == 1);
    check("shutdown_a2_a3_closed",
          fake_closes(FIX_A2) == 1 && fake_closes(FIX_A3) == 1);
    check("shutdown_disabled_fixture_closed", fake_closes(FIX_DISABLED) == 1);
    check("shutdown_no_use_after_close", g_fake_poison == 0);
}

/* The adopt path's owner tagging: an entry that registers and then fails must
 * have its registrations rolled back. */
static int adopt_rollback_entry(const AgcExtHost *host, AgcExt *out) {
    (void)out;
    host->add_tool(&rollback_tool);
    return -1;
}

static int adopt_bad_descriptor_entry(const AgcExtHost *host, AgcExt *out) {
    host->add_tool(&rollback_tool);
    fill_ext(out, "adoptroll2", "1.0", 0, NULL, NULL);
    out->abi_version = AGENTC_EXT_ABI + 1;
    return 0;
}

static void test_adopt_rollback(void) {
    agentc_ext_shutdown();
    check("adopt_rollback_rc", agentc_ext_adopt("adoptroll", adopt_rollback_entry) == -22);
    check("adopt_rollback_tools", collect_tools(NULL, 0) == 0);
    check("adopt_rollback_rc2",
          agentc_ext_adopt("adoptroll2", adopt_bad_descriptor_entry) == -22);
    check("adopt_rollback_tools2", collect_tools(NULL, 0) == 0);
}

static void test_disabled(void) {
    agentc_ext_shutdown();
    fake_reset_counts();
    g_load_n = 0;
    g_disabled_init_calls = 0;
    agentc_ext_register_dynamic();

    char *disabled[] = { (char *)"disabled_fx" };
    AgcConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);
    cfg.extensions_disabled = disabled;
    cfg.nextensions_disabled = 1;
    agentc_ext_apply_config(&cfg);
    agentc_ext_load_all();

    check("disabled_not_initialized", g_disabled_init_calls == 0);
    check("disabled_not_loaded", !agentc_ext_is_loaded("disabled_fx"));
    check("disabled_state", describe_state("disabled_fx") == AGENTC_EXT_STATE_SKIPPED);
    check("disabled_other_loaded", agentc_ext_is_loaded("sync"));
    agentc_ext_shutdown();
    check("disabled_closed_on_shutdown", fake_closes(FIX_DISABLED) == 1);
}

static void test_shutdown_busy(void) {
    agentc_ext_shutdown();
    fake_reset_counts();
    g_a2_shutdown_in_step = 1;
    g_a2_shutdown_called = 0;
    g_a2_closes_at_shutdown = -1;
    g_a2_stop_count = 0;
    g_a2_stop_reason = -1;
    agentc_ext_register_dynamic();
    agentc_ext_load_all();

    AgcTool tools[16];
    size_t ntools = collect_tools(tools, 16);
    RunCtx rc;
    run_tool_copy(find_tool("dyn.a2", tools, ntools), &rc);

    check("busy_shutdown_called", g_a2_shutdown_called == 1);
    check("busy_shutdown_no_close", g_a2_closes_at_shutdown == 0);
    check("busy_kept_mapped_forever", fake_closes(FIX_A2) == 0);
    check("busy_stop_unload", g_a2_stop_count == 1 &&
                                  g_a2_stop_reason == AGENTC_EXT_TOOL_UNLOAD);
    agentc_buf_free(&rc.out);
}

/* Regression: a dynamic extension's status provider unloads its own
 * library while a snapshot is calling it. The provider must run owner-tagged,
 * so the requested close is deferred past the provider call and lands at the
 * pump epilogue. Before the fix the raw provider ran with owner_get()==0 and
 * dyn_try_close() closed the handle inside the provider (the library's code was
 * still on the stack), which is the crash this test pins down. */
static void test_status_unload_in_provider(void) {
    agentc_ext_shutdown();
    fake_reset_counts();
    g_load_n = 0;
    g_su_provider_calls = 0;
    g_su_unloaded_in_provider = 0;
    g_su_closes_in_provider = -1;

    agentc_test_setenv("XDG_CONFIG_HOME", DYN_SU_XDG);
    agentc_ext_register_dynamic();
    agentc_ext_load_all();
    check("statusunload_loaded", agentc_ext_is_loaded("statusunload"));

    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(segs, 8);
    check("statusunload_provider_called", g_su_provider_calls == 1);
    check("statusunload_unloaded_mid_call", g_su_unloaded_in_provider == 1);
    check("statusunload_no_close_in_provider", g_su_closes_in_provider == 0);
    check("statusunload_segment_delivered",
          n == 1 && agentc_streq(segs[0].text, "su-seg"));

    /* The close requested while the provider was on the stack lands here. */
    agentc_ext_pump();
    check("statusunload_closed_after_pump", fake_closes(FIX_STATUSUNLOAD) == 1);
    check("statusunload_gone", !agentc_ext_is_loaded("statusunload"));
    agentc_test_setenv("XDG_CONFIG_HOME", DYN_XDG);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    size_t baseline = agentc_mem_live();
    agentc_test_clearenv();
    agentc_test_setenv("XDG_CONFIG_HOME", DYN_XDG);
    agentc_test_setenv("HOME", DYN_ROOT "/home");

    agentc_pump_install(test_pump, NULL);
    agentc_ext_dyn_set_loader(&g_fake_loader);

    test_loader_reject();
    make_tree();
    test_scan();
    test_path_ok();
    test_register_and_lifecycle();
    test_adopt_rollback();
    test_disabled();
    test_shutdown_busy();
    test_status_unload_in_provider();

    agentc_ext_shutdown();
    agentc_ext_dyn_set_loader(NULL);
    agentc_pump_install(NULL, NULL);
    fake_free_handles();
    agentc_test_clearenv();
    agentc_rm_rf(DYN_ROOT);
    check("mem_baseline", agentc_mem_live() == baseline);

    return fails;
}
