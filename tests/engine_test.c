/* engine_test.c — core-tool backend engine (P5-TOOL-ENGINE / redesign).
 *
 * Covers: parse, PATH resolution via a synthetic PATH (no host PATH), rung
 * selection, the pure per-backend descriptions, the argv mapping, the hit
 * normalizers (rg --json / POSIX grep, the shared emit formatter and its
 * 500-char cap), the output-byte budget, and (POSIX only) the external rungs
 * driven through agentc_tool_jobs_run with stub scripts: the grep chain
 * (ripgrep) and the find chain (fd -> POSIX find -> built-in).
 *
 * The golden file tests/data/engine_test.expected is shared with the Windows
 * build under Wine, so every printed line must be byte-identical on both:
 * platform-specific work is gated on os_platform() while the check() value is
 * kept the same, exactly like tools_test.c's read_devzero idiom. Nothing here
 * consults the host PATH or any installed rg/grep/fd.
 */
#include "agent.h"
#include "plat.h"
#include "base/limits.h"
#include "core/tools/engine.h"
#include "core/tools/jobs.h"

#define ROOT "/tmp/agentc-engine-test"

/* internal helper (not in the frozen headers), as used by tools2_test.c */
void agentc_rm_rf(const char *path);

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* --------------------------------------------------------------- helpers */
/* Own the buffer's storage and return a NUL-terminated string. */
static char *buf_to_str(AgcBuf *b) {
    agentc_buf_byte(b, 0);
    b->len--;
    if (b->p == NULL) return agentc_strdup_len("", 0);
    return (char *)b->p;
}

/* Print `label=` then the buffer with its trailing newline(s) removed, so the
 * exact normalized text is pinned in the golden without an extra blank line. */
static void print_normalized(const char *label, const AgcBuf *b) {
    agentc_outf("%s=", label);
    size_t n = b->len;
    while (n > 0 && (b->p[n - 1] == '\n' || b->p[n - 1] == '\r')) n--;
    if (b->p != NULL) agentc_out(b->p, n);
    agentc_out_nl();
}

static bool ends_with(const char *s, const char *suffix) {
    if (s == NULL) return false;
    size_t n = agentc_strlen(s);
    size_t m = agentc_strlen(suffix);
    return n >= m && agentc_memeq(s + n - m, suffix, m);
}

static void argv_free(AgcVec *v) {
    char **a = (char **)v->p;
    for (size_t i = 0; i < v->len; i++) agentc_free(a[i]);
    agentc_vec_free(v);
}

static bool argv_dashdash_before(const AgcVec *v, const char *pattern) {
    char **a = (char **)v->p;
    if (a == NULL) return false;
    for (size_t i = 0; a[i] != NULL; i++)
        if (agentc_streq(a[i], "--"))
            return a[i + 1] != NULL && agentc_streq(a[i + 1], pattern);
    return false;
}

static bool argv_has(const AgcVec *v, const char *arg) {
    char **a = (char **)v->p;
    for (size_t i = 0; a != NULL && a[i] != NULL; i++)
        if (agentc_streq(a[i], arg)) return true;
    return false;
}

static void print_argv(const char *label, const AgcVec *v) {
    AgcBuf b = { 0 };
    char **a = (char **)v->p;
    for (size_t i = 0; a != NULL && a[i] != NULL; i++) {
        if (i) agentc_buf_byte(&b, ' ');
        agentc_buf_cstr(&b, a[i]);
    }
    agentc_outf("%s=", label);
    if (b.p != NULL) agentc_out(b.p, b.len);
    agentc_out_nl();
    agentc_buf_free(&b);
}

/* Create a fresh executable file (the mode sets the execute bit on POSIX; the
 * unlink makes sure a stale file with a different mode is not reused). */
static void write_stub(const char *path, const char *body) {
    os_unlink(path);
    int fd = os_open(path, OS_O_CREAT | OS_O_WRONLY | OS_O_TRUNC, 0755);
    if (fd >= 0) {
        (void)os_write(fd, body, agentc_strlen(body));
        os_close(fd);
    }
}

static void write_fd_big_stub(const char *path, size_t pathlen) {
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "#!/bin/sh\nprintf '%s\\n' '");
    for (size_t i = 0; i < pathlen; i++) agentc_buf_byte(&b, 'p');
    agentc_buf_cstr(&b, "'\nwhile :; do :; done\n");
    agentc_buf_byte(&b, 0);
    write_stub(path, (const char *)b.p);
    agentc_buf_free(&b);
}

/* Generate an `fd` stub emitting exactly `count` paths, each `pathlen` bytes of
 * 'p'. Only shell builtins (empty environment), so it also works under a
 * synthetic PATH. */
static void write_fd_many_stub(const char *path, int count, size_t pathlen) {
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "#!/bin/sh\ni=0\nwhile test \"$i\" -lt ");
    agentc_buf_u64(&b, (u64)count);
    agentc_buf_cstr(&b, "; do\nprintf '%s\\n' '");
    for (size_t i = 0; i < pathlen; i++) agentc_buf_byte(&b, 'p');
    agentc_buf_cstr(&b, "'\ni=$((i + 1))\ndone\n");
    agentc_buf_byte(&b, 0);
    write_stub(path, (const char *)b.p);
    agentc_buf_free(&b);
}

/* Generate an `rg` stub emitting exactly `count` match records, each carrying
 * `textlen` bytes of text (path "f.txt", line 1). Only shell builtins (empty
 * environment), so it also works under a synthetic PATH. */
static void write_rg_many_stub(const char *path, int count, size_t textlen) {
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "#!/bin/sh\ni=0\nwhile test \"$i\" -lt ");
    agentc_buf_u64(&b, (u64)count);
    agentc_buf_cstr(&b, "; do\nprintf '%s\\n' '");
    agentc_buf_cstr(&b,
                    "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"f.txt\"},"
                    "\"line_number\":1,\"lines\":{\"text\":\"");
    for (size_t i = 0; i < textlen; i++) agentc_buf_byte(&b, 'x');
    agentc_buf_cstr(&b, "\"}}}'\ni=$((i + 1))\ndone\n");
    agentc_buf_byte(&b, 0);
    write_stub(path, (const char *)b.p);
    agentc_buf_free(&b);
}

static size_t count_byte(const char *s, char c) {
    size_t n = 0;
    for (; s != NULL && *s != 0; s++)
        if (*s == c) n++;
    return n;
}

/* ------------------------------------------------------------ test cases */
static void test_parse(void) {
    check("parse.external", agentc_tool_engine_parse("external") == AGENTC_TOOL_ENGINE_EXTERNAL);
    check("parse.internal", agentc_tool_engine_parse("internal") == AGENTC_TOOL_ENGINE_INTERNAL);
    check("parse.bogus", agentc_tool_engine_parse("bogus") == -1);
    check("parse.empty", agentc_tool_engine_parse("") == -1);
    check("parse.null", agentc_tool_engine_parse(NULL) == -1);
}

static void test_which(bool win) {
    char out[4096];

    /* Both names so the same case is found on POSIX (rg) and Windows (rg.exe /
     * exact rg); the resolver on Windows tries the exact name first, so either
     * file satisfies it. */
    write_stub(ROOT "/rg", "#!/bin/sh\nexit 1\n");
    write_stub(ROOT "/rg.exe", "#!/bin/sh\nexit 1\n");

    int r = os_which_in(ROOT, "rg", out, sizeof out);
    check("which.found", r == 0);
    /* POSIX and Windows both return an absolute path; the synthetic ROOT is
     * absolute on both (/tmp on Wine is the Z: drive root). */
    check("which.absolute", r == 0 && out[0] == '/');

    r = os_which_in(ROOT, "does-not-exist", out, sizeof out);
    check("which.missing", r == -2);

    /* No execute bit on POSIX; Windows marks executability by extension, so an
     * existing non-directory is a hit there. Same label, platform-adjusted
     * expectation (tools_test.c read_devzero idiom). */
    os_unlink(ROOT "/noexec");
    int fd = os_open(ROOT "/noexec", OS_O_CREAT | OS_O_WRONLY | OS_O_TRUNC, 0644);
    if (fd >= 0) os_close(fd);
    r = os_which_in(ROOT, "noexec", out, sizeof out);
    check("which.nonexec", win ? (r == 0) : (r == -2));

    r = os_which_in(ROOT, "rg", out, 1);
    check("which.cap", r == -34);

    r = os_which_in(NULL, "rg", out, sizeof out);
    check("which.null-path", r == -2);
    r = os_which_in("", "rg", out, sizeof out);
    check("which.empty-path", r == -2);
}

static void test_selection(void) {
    AgcToolEngineSel sel;

    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT);
    check("select.external.grep", agentc_streq(agentc_tool_engine_backend("grep"), AGENTC_ENGINE_RG));
    check("select.external.select", agentc_tool_engine_select("grep", &sel) && sel.run != NULL);
    check("select.external.desc",
          sel.desc != NULL &&
              agentc_streq(sel.desc, agentc_tool_engine_desc_for("grep", AGENTC_ENGINE_RG)));

    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, "");
    check("select.empty.grep",
          agentc_streq(agentc_tool_engine_backend("grep"), AGENTC_ENGINE_BUILTIN));
    check("select.empty.run-null", agentc_tool_engine_select("grep", &sel) && sel.run == NULL);

    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_INTERNAL, ROOT);
    check("select.internal.grep",
          agentc_streq(agentc_tool_engine_backend("grep"), AGENTC_ENGINE_BUILTIN));
    check("select.internal.run-null", agentc_tool_engine_select("grep", &sel) && sel.run == NULL);
    check("select.internal.find-run-null",
          agentc_tool_engine_select("find", &sel) && sel.run == NULL);
    check("select.read-false", !agentc_tool_engine_select("read", &sel));
}

/* find chain (T2): fd -> POSIX find -> built-in, POSIX-only for the middle
 * rung. Uses stub-only synthetic PATHs so the host is irrelevant. */
static void test_find_selection(bool win) {
    AgcToolEngineSel sel;

    write_stub(ROOT "/onlyfd/fd", "#!/bin/sh\nexit 1\n");
    write_stub(ROOT "/onlyfd/fd.exe", "#!/bin/sh\nexit 1\n");
    write_stub(ROOT "/onlyfind/find", "#!/bin/sh\nexit 1\n");
    write_stub(ROOT "/onlyfind/find.exe", "#!/bin/sh\nexit 1\n");

    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT "/onlyfd");
    check("select.find.fd", agentc_streq(agentc_tool_engine_backend("find"), AGENTC_ENGINE_FD));
    check("select.find.fd.run", agentc_tool_engine_select("find", &sel) && sel.run != NULL);

    /* POSIX find is never selected on Windows (find.exe is a text searcher):
     * the chain there is fd -> built-in. Same labels, platform-adjusted value. */
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT "/onlyfind");
    check("select.find.posix",
          win ? agentc_streq(agentc_tool_engine_backend("find"), AGENTC_ENGINE_BUILTIN)
              : agentc_streq(agentc_tool_engine_backend("find"), AGENTC_ENGINE_FIND));
    check("select.find.posix.run",
          win ? (agentc_tool_engine_select("find", &sel) && sel.run == NULL)
              : (agentc_tool_engine_select("find", &sel) && sel.run != NULL));

    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, "");
    check("select.find.empty",
          agentc_streq(agentc_tool_engine_backend("find"), AGENTC_ENGINE_BUILTIN));
    check("select.find.empty.run-null", agentc_tool_engine_select("find", &sel) && sel.run == NULL);

    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_INTERNAL, ROOT "/onlyfd");
    check("select.find.internal",
          agentc_streq(agentc_tool_engine_backend("find"), AGENTC_ENGINE_BUILTIN));
    check("select.find.internal.run-null",
          agentc_tool_engine_select("find", &sel) && sel.run == NULL);
}

/* Pure: pins every backend's text without depending on the host PATH. */
static void test_desc(void) {
    agentc_outf("desc.grep.ripgrep=%s\n", agentc_tool_engine_desc_for("grep", "ripgrep"));
    agentc_outf("desc.grep.grep=%s\n", agentc_tool_engine_desc_for("grep", "grep"));
    agentc_outf("desc.grep.builtin=%s\n", agentc_tool_engine_desc_for("grep", "built-in"));
    agentc_outf("desc.find.fd=%s\n", agentc_tool_engine_desc_for("find", "fd"));
    agentc_outf("desc.find.posix=%s\n", agentc_tool_engine_desc_for("find", "find"));
    agentc_outf("desc.find.builtin=%s\n", agentc_tool_engine_desc_for("find", "built-in"));
}

static void test_argv(void) {
    AgcGrepArgs ga;
    AgcVec v;

    /* full ripgrep flag set */
    agentc_memset(&ga, 0, sizeof ga);
    ga.pattern = "foo";
    ga.path = ".";
    ga.icase = true;
    ga.literal = false;
    ga.glob = "*.c";
    ga.context = 2;
    ga.limit = 5;
    agentc_memset(&v, 0, sizeof v);
    int r = agentc_tool_engine_build_argv("grep", AGENTC_ENGINE_RG, &ga, &v);
    print_argv("argv.rg.full", &v);
    check("argv.rg.full", r == 0 && argv_dashdash_before(&v, "foo"));
    argv_free(&v);

    /* ripgrep literal, minimal flags, and a '-' pattern that must stay positional */
    agentc_memset(&ga, 0, sizeof ga);
    ga.pattern = "-bar";
    ga.literal = true;
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("grep", AGENTC_ENGINE_RG, &ga, &v);
    print_argv("argv.rg.literal", &v);
    check("argv.rg.literal", r == 0 && argv_dashdash_before(&v, "-bar"));
    argv_free(&v);

    /* full POSIX grep flag set (-E, not -F) */
    agentc_memset(&ga, 0, sizeof ga);
    ga.pattern = "foo";
    ga.path = ".";
    ga.icase = true;
    ga.literal = false;
    ga.glob = "*.c";
    ga.context = 2;
    ga.limit = 5;
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("grep", AGENTC_ENGINE_GREP, &ga, &v);
    print_argv("argv.grep.full", &v);
    check("argv.grep.full", r == 0 && argv_dashdash_before(&v, "foo"));
    /* Guard: the pinned POSIX rung has no fallback, so it must use the
     * portable short options (-i/-C/-m), never the GNU long forms. argv
     * building is platform-independent, so this value is the same on Wine. */
    check("argv.grep.portable",
          argv_has(&v, "-i") && argv_has(&v, "-C") && argv_has(&v, "-m") &&
              !argv_has(&v, "--ignore-case") && !argv_has(&v, "--context") &&
              !argv_has(&v, "--max-count"));
    argv_free(&v);

    /* literal grep switches -E to -F; minimal set uses the defaults */
    agentc_memset(&ga, 0, sizeof ga);
    ga.pattern = "bar";
    ga.literal = true;
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("grep", AGENTC_ENGINE_GREP, &ga, &v);
    print_argv("argv.grep.literal", &v);
    check("argv.grep.literal", r == 0 && argv_dashdash_before(&v, "bar"));
    argv_free(&v);

    /* G2: null_sep adds -Z right after -H; the default golden above stays
     * byte-identical (no -Z). */
    agentc_memset(&ga, 0, sizeof ga);
    ga.pattern = "foo";
    ga.path = ".";
    ga.null_sep = true;
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("grep", AGENTC_ENGINE_GREP, &ga, &v);
    print_argv("argv.grep.null", &v);
    check("argv.grep.null", r == 0 && argv_has(&v, "-Z") && argv_dashdash_before(&v, "foo"));
    argv_free(&v);

    /* fd: pattern is positional after `--`; full flags carry --max-results N+1
     * (the reader drops the extra path). */
    AgcFindArgs fa;
    agentc_memset(&fa, 0, sizeof fa);
    fa.pattern = "*.c";
    fa.path = "src";
    fa.limit = 5;
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("find", AGENTC_ENGINE_FD, &fa, &v);
    print_argv("argv.fd.full", &v);
    check("argv.fd.full", r == 0 && argv_dashdash_before(&v, "*.c"));
    argv_free(&v);

    /* default path and limit -> --max-results 1001 and path "." */
    agentc_memset(&fa, 0, sizeof fa);
    fa.pattern = "*.c";
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("find", AGENTC_ENGINE_FD, &fa, &v);
    print_argv("argv.fd.default", &v);
    check("argv.fd.default", r == 0 && argv_dashdash_before(&v, "*.c"));
    argv_free(&v);

    /* POSIX find: no slash -> -name on the basename; a slash -> -path verbatim */
    agentc_memset(&fa, 0, sizeof fa);
    fa.pattern = "*.c";
    fa.path = "src";
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("find", AGENTC_ENGINE_FIND, &fa, &v);
    print_argv("argv.find.name", &v);
    check("argv.find.name", r == 0 && argv_has(&v, "-H") && argv_has(&v, "-name") &&
                                 argv_has(&v, "-path") == false &&
                                 ((char **)v.p)[2] != NULL &&
                                 agentc_streq(((char **)v.p)[2], "src"));
    argv_free(&v);

    agentc_memset(&fa, 0, sizeof fa);
    fa.pattern = "src/*.c";
    fa.path = "src";
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("find", AGENTC_ENGINE_FIND, &fa, &v);
    print_argv("argv.find.path", &v);
    check("argv.find.path", r == 0 && argv_has(&v, "-H") && argv_has(&v, "-path"));
    argv_free(&v);

    /* RC3 regression guard: a path that starts with an expression token must be
     * anchored with `./`, so POSIX find can never turn it into an action such as
     * -delete; the pattern still follows -name. */
    {
        static const char *const anchor_paths[] = { "-delete", "!x", "(", ")" };
        static const char *const anchor_labels[] = { "argv.find.anchor-dash",
                                                     "argv.find.anchor-bang",
                                                     "argv.find.anchor-open",
                                                     "argv.find.anchor-close" };
        for (size_t i = 0; i < sizeof anchor_paths / sizeof anchor_paths[0]; i++) {
            agentc_memset(&fa, 0, sizeof fa);
            fa.pattern = "*.c";
            fa.path = anchor_paths[i];
            agentc_memset(&v, 0, sizeof v);
            r = agentc_tool_engine_build_argv("find", AGENTC_ENGINE_FIND, &fa, &v);
            print_argv(anchor_labels[i], &v);
            char **a = (char **)v.p;
            check(anchor_labels[i],
                  r == 0 && a != NULL && a[1] != NULL && agentc_streq(a[1], "-H") &&
                      a[2] != NULL && agentc_str_starts(a[2], 2, "./") && a[3] != NULL &&
                      agentc_streq(a[3], "-name") && a[4] != NULL &&
                      agentc_streq(a[4], "*.c"));
            argv_free(&v);
        }
    }

    /* the built-in (and any unknown) backend has no external argv */
    agentc_memset(&fa, 0, sizeof fa);
    fa.pattern = "*.c";
    agentc_memset(&v, 0, sizeof v);
    r = agentc_tool_engine_build_argv("find", AGENTC_ENGINE_BUILTIN, &fa, &v);
    check("argv.find.einval", r == -22);
    argv_free(&v);
}

/* Ported line-normalizer labels: parse a backend line, then format the hit
 * through the shared emit_hit (one JSON arena reused across the rg cases). */
static void test_rg_line(void) {
    static const char match[] =
        "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"a.txt\"},\"line_number\":3,"
        "\"lines\":{\"text\":\"hello\\n\"}}}";
    static const char context[] =
        "{\"type\":\"context\",\"data\":{\"path\":{\"text\":\"a.txt\"},\"line_number\":4,"
        "\"lines\":{\"text\":\"world\\n\"}}}";
    static const char begin[] = "{\"type\":\"begin\",\"data\":{\"path\":{\"text\":\"a.txt\"}}}";
    static const char summary[] =
        "{\"type\":\"summary\",\"data\":{\"elapsed_total\":{\"secs\":0,\"nanos\":1},"
        "\"stats\":{\"matches\":1}}}";

    AgcJsonArena *scratch = agentc_json_arena_new(0);
    AgcGrepHit h;
    AgcBuf o = { 0 };

    int r = agentc_tool_engine_rg_hit(scratch, match, agentc_strlen(match), &h);
    if (r) agentc_tool_engine_emit_hit(&o, &h);
    print_normalized("rg.match", &o);
    check("rg.match", r == 1 && h.is_match);
    agentc_buf_clear(&o);

    r = agentc_tool_engine_rg_hit(scratch, context, agentc_strlen(context), &h);
    if (r) agentc_tool_engine_emit_hit(&o, &h);
    print_normalized("rg.context", &o);
    check("rg.context", r == 1 && !h.is_match);
    agentc_buf_clear(&o);

    r = agentc_tool_engine_rg_hit(scratch, begin, agentc_strlen(begin), &h);
    check("rg.begin", r == 0 && o.len == 0);
    r = agentc_tool_engine_rg_hit(scratch, summary, agentc_strlen(summary), &h);
    check("rg.summary", r == 0 && o.len == 0);
    r = agentc_tool_engine_rg_hit(scratch, "not json", 8, &h);
    check("rg.malformed", r == 0 && o.len == 0);

    /* 500-char cap + "..." on a 600-char text */
    AgcBuf j = { 0 };
    agentc_buf_cstr(&j,
                    "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"long.txt\"},"
                    "\"line_number\":7,\"lines\":{\"text\":\"");
    for (int i = 0; i < 600; i++) agentc_buf_byte(&j, 'x');
    agentc_buf_cstr(&j, "\"}}}");
    AgcBuf capb = { 0 };
    r = agentc_tool_engine_rg_hit(scratch, (const char *)j.p, j.len, &h);
    check("rg.cap.return", r == 1 && h.is_match);
    if (r) agentc_tool_engine_emit_hit(&capb, &h);
    check("rg.cap.len", capb.len == 515);
    check("rg.cap.ellipsis", capb.len >= 4 && agentc_memeq(capb.p + capb.len - 4, "...\n", 4));
    check("rg.cap.prefix", agentc_str_starts((const char *)capb.p, capb.len, "long.txt:7:"));
    agentc_buf_free(&j);
    agentc_buf_free(&capb);
    agentc_buf_free(&o);
    agentc_json_arena_free(scratch);
}

static void test_grep_line(void) {
    static const char match[] = "file.txt:12:some text\n";
    static const char ctx[] = "file.txt-13-context text\n";
    static const char blank[] = "\n";
    static const char none[] = "plain text\n";

    AgcGrepHit h;
    AgcBuf o = { 0 };
    int r = agentc_tool_engine_grep_hit(match, agentc_strlen(match), &h);
    if (r) agentc_tool_engine_emit_hit(&o, &h);
    print_normalized("grep.match", &o);
    check("grep.match", r == 1 && h.is_match);
    agentc_buf_clear(&o);

    r = agentc_tool_engine_grep_hit(ctx, agentc_strlen(ctx), &h);
    if (r) agentc_tool_engine_emit_hit(&o, &h);
    print_normalized("grep.context", &o);
    check("grep.context", r == 1 && !h.is_match);
    agentc_buf_clear(&o);

    /* G2: NUL-delimited `grep -H -Z` records. The path is everything before the
     * first NUL, so a name like report-2024-q1.txt cannot be mis-split. */
    static const char gnull_match[] = "report-2024-q1.txt\0" "2:alpha here\n";
    static const char gnull_ctx[] = "a.txt\0" "9-before\n";
    r = agentc_tool_engine_grep_hit_null(gnull_match, sizeof gnull_match - 1, &h);
    if (r) agentc_tool_engine_emit_hit(&o, &h);
    print_normalized("grep.nul.match", &o);
    check("grep.nul.match",
          r == 1 && h.is_match && h.line == 2 && h.path_len == 18 &&
              agentc_memeq(h.path, "report-2024-q1.txt", 18) && h.text_len == 10 &&
              agentc_memeq(h.text, "alpha here", 10));
    agentc_buf_clear(&o);

    r = agentc_tool_engine_grep_hit_null(gnull_ctx, sizeof gnull_ctx - 1, &h);
    if (r) agentc_tool_engine_emit_hit(&o, &h);
    print_normalized("grep.nul.context", &o);
    check("grep.nul.context", r == 1 && !h.is_match && h.line == 9 && h.text_len == 6);
    agentc_buf_clear(&o);

    r = agentc_tool_engine_grep_hit(blank, agentc_strlen(blank), &h);
    check("grep.blank", r == 0 && o.len == 0);
    r = agentc_tool_engine_grep_hit(none, agentc_strlen(none), &h);
    check("grep.none", r == 0 && o.len == 0);

    /* 500-char cap + "..." on a 600-char text */
    AgcBuf j = { 0 };
    agentc_buf_cstr(&j, "c.txt:1:");
    for (int i = 0; i < 600; i++) agentc_buf_byte(&j, 'x');
    agentc_buf_byte(&j, '\n');
    AgcBuf capb = { 0 };
    r = agentc_tool_engine_grep_hit((const char *)j.p, j.len, &h);
    check("grep.cap.return", r == 1);
    if (r) agentc_tool_engine_emit_hit(&capb, &h);
    check("grep.cap.len", capb.len == 512);
    check("grep.cap.ellipsis", capb.len >= 4 && agentc_memeq(capb.p + capb.len - 4, "...\n", 4));
    agentc_buf_free(&j);
    agentc_buf_free(&capb);
    agentc_buf_free(&o);
}

/* The redesigned hit seam: pin the parsed record fields directly (not just the
 * formatted text), then pin the shared emit_hit 500-char cap in one place. */
static void test_hits(void) {
    static const char rmatch[] =
        "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"a.txt\"},\"line_number\":3,"
        "\"lines\":{\"text\":\"hello\\n\"}}}";
    static const char rctx[] =
        "{\"type\":\"context\",\"data\":{\"path\":{\"text\":\"a.txt\"},\"line_number\":4,"
        "\"lines\":{\"text\":\"world\\n\"}}}";
    static const char rbegin[] = "{\"type\":\"begin\",\"data\":{\"path\":{\"text\":\"a.txt\"}}}";
    static const char rbad[] = "not json";

    AgcJsonArena *scratch = agentc_json_arena_new(0);
    AgcGrepHit h;
    int r = agentc_tool_engine_rg_hit(scratch, rmatch, agentc_strlen(rmatch), &h);
    check("hit.rg.match",
          r == 1 && h.is_match && h.line == 3 && h.path_len == 5 &&
              agentc_memeq(h.path, "a.txt", 5) && h.text_len == 5 &&
              agentc_memeq(h.text, "hello", 5));

    r = agentc_tool_engine_rg_hit(scratch, rctx, agentc_strlen(rctx), &h);
    check("hit.rg.context",
          r == 1 && !h.is_match && h.line == 4 && h.text_len == 5 &&
              agentc_memeq(h.text, "world", 5));

    r = agentc_tool_engine_rg_hit(scratch, rbegin, agentc_strlen(rbegin), &h);
    check("hit.rg.begin", r == 0);

    r = agentc_tool_engine_rg_hit(scratch, rbad, agentc_strlen(rbad), &h);
    check("hit.rg.malformed", r == 0);
    agentc_json_arena_free(scratch);

    static const char gmatch[] = "file.txt:12:some text\n";
    static const char gctx[] = "file.txt-13-context text\n";
    r = agentc_tool_engine_grep_hit(gmatch, agentc_strlen(gmatch), &h);
    check("hit.grep.match",
          r == 1 && h.is_match && h.line == 12 && h.path_len == 8 &&
              agentc_memeq(h.path, "file.txt", 8) && h.text_len == 9 &&
              agentc_memeq(h.text, "some text", 9));

    r = agentc_tool_engine_grep_hit(gctx, agentc_strlen(gctx), &h);
    check("hit.grep.context",
          r == 1 && !h.is_match && h.line == 13 && h.text_len == 12 &&
              agentc_memeq(h.text, "context text", 12));

    /* G2: the NUL-delimited parser pins the record fields directly. */
    static const char gnul[] = "report-2024-q1.txt\0" "2:alpha here\n";
    static const char gnul_ctx[] = "a.txt\0" "9-before\n";
    static const char gno_nul[] = "plain:1:x\n";
    static const char gno_num[] = "a.txt\0no-digit\n";
    static const char gsep[] = "--\n";
    r = agentc_tool_engine_grep_hit_null(gnul, sizeof gnul - 1, &h);
    check("hit.grep.nul.match",
          r == 1 && h.is_match && h.line == 2 && h.path_len == 18 &&
              agentc_memeq(h.path, "report-2024-q1.txt", 18) && h.text_len == 10 &&
              agentc_memeq(h.text, "alpha here", 10));
    r = agentc_tool_engine_grep_hit_null(gnul_ctx, sizeof gnul_ctx - 1, &h);
    check("hit.grep.nul.context",
          r == 1 && !h.is_match && h.line == 9 && h.text_len == 6 &&
              agentc_memeq(h.text, "before", 6));
    r = agentc_tool_engine_grep_hit_null(gno_nul, sizeof gno_nul - 1, &h);
    check("hit.grep.nul.no-nul", r == 0);
    r = agentc_tool_engine_grep_hit_null(gno_num, sizeof gno_num - 1, &h);
    check("hit.grep.nul.no-number", r == 0);
    r = agentc_tool_engine_grep_hit_null(gsep, sizeof gsep - 1, &h);
    check("hit.grep.nul.separator", r == 0);

    /* the one emit formatter applies AGENTC_LIMIT_GREP_LINE with "..." */
    char big[600];
    for (size_t i = 0; i < sizeof big; i++) big[i] = 'x';
    AgcGrepHit cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.path = "long.txt";
    cap.path_len = 8;
    cap.line = 7;
    cap.text = big;
    cap.text_len = sizeof big;
    cap.is_match = true;
    AgcBuf o = { 0 };
    agentc_tool_engine_emit_hit(&o, &cap);
    check("emit.cap",
          o.len == 515 && agentc_memeq(o.p + o.len - 4, "...\n", 4) &&
              agentc_memeq(o.p, "long.txt:7:", 11));
    agentc_buf_free(&o);
}

/* ------------------------------------------------- external spawn (POSIX) */
typedef struct {
    AgcBuf out;
    bool is_error;
} RunCtx;

static int run_collect(void *ud, int what, const AgcJob *job) {
    RunCtx *c = ud;
    if (what != AGENTC_JOB_DONE) return 0;
    if (job->out.len > 0) agentc_buf_push(&c->out, job->out.p, job->out.len);
    c->is_error = job->is_error;
    return 0;
}

/* Drive one sync run through the production job driver, exactly like
 * tools_test.c does for the async tools, so the timeout/deadline path is the
 * real one. */
static char *run_external(const AgcTool *t, const char *args, bool *is_error) {
    AgcToolCall call = { "test", t->name, args, NULL };
    RunCtx c;
    agentc_memset(&c, 0, sizeof c);
    const AgcTool *tools[1] = { t };
    (void)agentc_tool_jobs_run(tools, NULL, &call, 1, run_collect, &c);
    if (is_error) *is_error = c.is_error;
    return buf_to_str(&c.out);
}

/* One-shot pump observer: while a synchronous tool spins, the engine's
 * proc_pump calls agentc_pump() each loop; once `after_ms` has elapsed the
 * observer flips the job's cancel flag, so the run stops cooperatively
 * (cancelled, never timed out). */
typedef struct {
    volatile bool *cancel;
    i64 start_ns;
    int after_ms;
} CancelTimer;

static void cancel_timer_pump(void *ud, int timeout_ms) {
    (void)timeout_ms;
    CancelTimer *ct = ud;
    if (ct == NULL || ct->cancel == NULL) return;
    if ((os_now_ns(OS_CLOCK_MONOTONIC) - ct->start_ns) / 1000000 >= ct->after_ms)
        *ct->cancel = true;
}

/* Drive one sync run through the production job driver and cancel it from a
 * short pump timer while the stub spins. Like run_external, but the cancel
 * flag flips mid-run, exercising the cancelled (not timed-out) path. */
static char *run_cancel_external(const AgcTool *t, const char *args, int after_ms,
                                 bool *is_error) {
    volatile bool cancel = false;
    AgcToolCall call = { "test", t->name, args, &cancel };
    RunCtx c;
    agentc_memset(&c, 0, sizeof c);
    CancelTimer ct;
    agentc_memset(&ct, 0, sizeof ct);
    ct.cancel = &cancel;
    ct.start_ns = os_now_ns(OS_CLOCK_MONOTONIC);
    ct.after_ms = after_ms;
    agentc_pump_install(cancel_timer_pump, &ct);
    const AgcTool *tools[1] = { t };
    (void)agentc_tool_jobs_run(tools, NULL, &call, 1, run_collect, &c);
    agentc_pump_install(NULL, NULL);
    if (is_error) *is_error = c.is_error;
    return buf_to_str(&c.out);
}

/* Children run with an explicit empty environment, so the stubs use only
 * absolute paths and shell builtins (no PATH lookup, no sleep). */
static const char STUB_TWO[] =
    "#!/bin/sh\n"
    "printf '%s\\n' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"one.txt\"},\"line_number\":1,"
    "\"lines\":{\"text\":\"alpha\\n\"}}}' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"two.txt\"},\"line_number\":2,"
    "\"lines\":{\"text\":\"beta\"}}}'\n"
    "exit 0\n";

/* exit 1 with no output: rg's "no matches", not an error */
static const char STUB_EMPTY[] = "#!/bin/sh\nexit 1\n";

/* exit 2 + stderr: a real failure whose text must name the backend */
static const char STUB_ERROR[] = "#!/bin/sh\necho boom >&2\nexit 2\n";

/* One match past `limit` (so the reader observes the dropped match), then it
 * keeps running: the reader must drop the extra match, stop at the cap, kill
 * the still-live child and append the truncation marker. */
static const char STUB_LIMIT[] =
    "#!/bin/sh\n"
    "printf '%s\\n' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"one.txt\"},\"line_number\":1,"
    "\"lines\":{\"text\":\"alpha\"}}}' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"two.txt\"},\"line_number\":2,"
    "\"lines\":{\"text\":\"beta\"}}}' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"three.txt\"},\"line_number\":3,"
    "\"lines\":{\"text\":\"gamma\"}}}'\n"
    "while :; do :; done\n";

/* limit 1, context 1: match A line 10 is accepted with its ±1 window (lines 9
 * and 11); context line 19 immediately precedes the rejected (limit+1)-th match
 * B line 20 and must NOT be emitted, even though it was buffered before B was
 * seen. */
static const char STUB_CTX_DROP[] =
    "#!/bin/sh\n"
    "printf '%s\\n' "
    "'{\"type\":\"context\",\"data\":{\"path\":{\"text\":\"f.txt\"},\"line_number\":9,"
    "\"lines\":{\"text\":\"before-A\"}}}' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"f.txt\"},\"line_number\":10,"
    "\"lines\":{\"text\":\"AAA\"}}}' "
    "'{\"type\":\"context\",\"data\":{\"path\":{\"text\":\"f.txt\"},\"line_number\":11,"
    "\"lines\":{\"text\":\"after-A\"}}}' "
    "'{\"type\":\"context\",\"data\":{\"path\":{\"text\":\"f.txt\"},\"line_number\":19,"
    "\"lines\":{\"text\":\"before-B\"}}}' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"f.txt\"},\"line_number\":20,"
    "\"lines\":{\"text\":\"BBB\"}}}'\n"
    "exit 0\n";

/* never writes: the 150 ms deadline must fire and the group must be reaped */
static const char STUB_BUSY[] =
    "#!/bin/sh\n"
    "echo $$ > " ROOT "/busy.pid\n"
    "while :; do :; done\n";

/* one complete record with NO trailing newline: the post-pump flush must still
 * process it */
static const char STUB_RG_EOF[] =
    "#!/bin/sh\n"
    "printf '%s' "
    "'{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"eof.txt\"},\"line_number\":5,"
    "\"lines\":{\"text\":\"flushed\"}}}'\n"
    "exit 0\n";

/* find stubs. The reader sorts collected paths bytewise; the limit stub emits
 * one path past `limit` then spins, so the reader must drop it, stop and kill. */
static const char STUB_FD_MULTI[] =
    "#!/bin/sh\nprintf '%s\\n' b.txt a.txt c.txt\nexit 0\n";
static const char STUB_FD_EMPTY[] = "#!/bin/sh\nexit 1\n";
static const char STUB_FD_LIMIT[] =
    "#!/bin/sh\nprintf '%s\\n' b.txt a.txt c.txt\nwhile :; do :; done\n";
static const char STUB_FD_ERROR[] = "#!/bin/sh\necho boom >&2\nexit 2\n";
/* a "\r"-only line must be trimmed before the emptiness test */
static const char STUB_FD_BLANK[] =
    "#!/bin/sh\nprintf 'a.txt\\n'\nprintf '\\r\\n'\nprintf 'b.txt\\n'\nexit 0\n";
/* one path with NO trailing newline: the post-pump flush must still process it */
static const char STUB_FD_EOF[] = "#!/bin/sh\nprintf '%s' 'eof.txt'\nexit 0\n";
/* emits one path, records its pid, then spins: a pump-driven cancel must keep
 * the partial path, add no marker and leave no live child behind */
static const char STUB_FD_CANCEL[] =
    "#!/bin/sh\n"
    "echo $$ > " ROOT "/cancel.pid\n"
    "printf '%s\\n' 'cancel.txt'\n"
    "while :; do :; done\n";
static const char STUB_FIND_MULTI[] =
    "#!/bin/sh\nprintf '%s\\n' z.txt y.txt\nexit 0\n";
static const char STUB_FIND_ERROR[] = "#!/bin/sh\necho bang >&2\nexit 2\n";
/* POSIX find treats exit 1 as a real failure (unlike fd, where 1 = no match). */
static const char STUB_FIND_EXIT1_ERR[] = "#!/bin/sh\necho boom >&2\nexit 1\n";
/* fd marks a directory with a trailing slash; the reader must strip it. */
static const char STUB_FD_DIRSLASH[] =
    "#!/bin/sh\nprintf '%s\\n' 'dir/' 'z.txt'\nexit 0\n";
/* POSIX find echoes its search operand; this stub prints the operand and one
 * child, so a directory operand must be dropped while a file operand is kept. */
static const char STUB_FIND_ROOT[] =
    "#!/bin/sh\nprintf '%s\\n' \"$2\" \"$2/child.txt\"\nexit 0\n";
static const char STUB_FIND_OPERAND[] = "#!/bin/sh\nprintf '%s\\n' \"$2\"\nexit 0\n";
/* grep -Z probe + real run: emit a NUL-delimited record only when -Z is
 * present, so the probe (which requires a NUL on stdout) sees support. */
static const char STUB_GREP_NULL[] =
    "#!/bin/sh\n"
    "hasz=\n"
    "for a in \"$@\"; do test \"$a\" = \"-Z\" && hasz=1; done\n"
    "if [ -n \"$hasz\" ]; then\n"
    "  printf 'report-2024-q1.txt\\0002:alpha here\\n'\n"
    "else\n"
    "  printf 'report-2024-q1.txt:2:alpha here\\n'\n"
    "fi\n"
    "exit 0\n";
/* grep without -Z: reject -Z with exit 2 for the probe and for any real run
 * (proving the fallback omits it); otherwise a textual record. */
static const char STUB_GREP_FALLBACK[] =
    "#!/bin/sh\n"
    "for a in \"$@\"; do test \"$a\" = \"-Z\" && exit 2; done\n"
    "printf 'normal.txt:1:alpha\\n'\n"
    "exit 0\n";
/* A grep whose usage error exits 1 (BusyBox-style): the probe must NOT trust
 * the exit code -- it waits for a NUL on stdout, sees none, and falls back to
 * the text parser (D1 regression guard). */
static const char STUB_GREP_USAGE_EXIT1[] =
    "#!/bin/sh\n"
    "for a in \"$@\"; do test \"$a\" = \"-Z\" && exit 1; done\n"
    "printf 'normal.txt:1:alpha\\n'\n"
    "exit 0\n";
/* Empty-pattern calls must be rejected before any spawn: these stubs record a
 * sentinel (and would print garbage) if they ever ran. */
static const char STUB_MARK_RG[] =
    "#!/bin/sh\n"
    "echo ran > " ROOT "/ran-rg\n"
    "printf 'SHOULD-NOT-RUN\\n'\n"
    "exit 0\n";
static const char STUB_MARK_FD[] =
    "#!/bin/sh\n"
    "echo ran > " ROOT "/ran-fd\n"
    "printf 'SHOULD-NOT-RUN\\n'\n"
    "exit 0\n";

static void test_spawn(bool win) {
    const char *args = "{\"pattern\":\"alpha\"}";
    const char *args_limit = "{\"pattern\":\"alpha\",\"limit\":2}";
    const char *args_ctx = "{\"pattern\":\"x\",\"limit\":1,\"context\":1}";
    /* fit.no-silent-trim: the largest max-length-line batch whose reader-side
     * accounting (path+text+32/record) still fits under AGENTC_LIMIT_TOOL_BYTES */
    const char *args_fit = "{\"pattern\":\"x\",\"limit\":93}";

    if (win) {
        /* Documented skip: Windows cannot exec a POSIX script. Print the same
         * label values as the POSIX run so the shared golden stays identical. */
        check("spawn.select", true);
        check("spawn.two", true);
        check("spawn.none", true);
        check("spawn.error", true);
        check("spawn.limit", true);
        check("spawn.limit.stopped", true);
        check("ctx.dropped", true);
        check("out.cap", true);
        check("longline.prefix", true);
        check("eof.flush.grep", true);
        check("fit.no-silent-trim", true);
        check("spawn.timeout", true);
        check("spawn.timeout.fast", true);
        check("spawn.timeout.child-gone", true);
        check("spawn.grep.null.select", true);
        check("spawn.grep.null", true);
        check("spawn.grep.plain.fallback", true);
        check("spawn.grep.usage-exit1", true);
        return;
    }

    AgcToolEngineSel sel;
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT);
    check("spawn.select", agentc_tool_engine_select("grep", &sel) && sel.run != NULL);

    AgcTool t;
    agentc_memset(&t, 0, sizeof t);
    t.name = "grep";
    t.run = sel.run;

    bool err = false;
    char *res;

    write_stub(ROOT "/rg", STUB_TWO);
    res = run_external(&t, args, &err);
    check("spawn.two",
          !err && res != NULL && agentc_streq(res, "one.txt:1:alpha\ntwo.txt:2:beta\n"));
    agentc_free(res);

    write_stub(ROOT "/rg", STUB_EMPTY);
    res = run_external(&t, args, &err);
    check("spawn.none", !err && res != NULL && agentc_streq(res, "no matches\n"));
    agentc_free(res);

    write_stub(ROOT "/rg", STUB_ERROR);
    res = run_external(&t, args, &err);
    check("spawn.error",
          err && res != NULL && agentc_streq(res, "error: grep: ripgrep: boom\n"));
    agentc_free(res);

    /* The stub emits exactly the cap and then spins; the reader must stop at
     * the cap (not wait for it) and mark the truncation. */
    write_stub(ROOT "/rg", STUB_LIMIT);
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, args_limit, &err);
    i64 ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("spawn.limit",
          !err && res != NULL &&
              agentc_streq(res, "one.txt:1:alpha\ntwo.txt:2:beta\n[grep truncated at 2 matches]\n"));
    check("spawn.limit.stopped", ms < 10000);
    agentc_free(res);

    /* Record-based context: the accepted match's ±1 window (before-A/after-A)
     * is kept; before-B, the context of the rejected (limit+1)-th match, is
     * dropped. */
    write_stub(ROOT "/rg", STUB_CTX_DROP);
    res = run_external(&t, args_ctx, &err);
    check("ctx.dropped",
          !err && res != NULL &&
              agentc_streq(res, "f.txt:9:before-A\nf.txt:10:AAA\nf.txt:11:after-A\n"
                                 "[grep truncated at 1 matches]\n"));
    agentc_free(res);

    /* Output-byte budget: many 500-byte match records are charged
     * path+text+32 = 537 at the reader, so the 94th crosses
     * AGENTC_LIMIT_TOOL_BYTES; the reader stops at the cap, kills the still-live
     * child and appends the byte marker. The stub is finite, so the run ends on
     * its own instead of spinning to the timeout. */
    write_rg_many_stub(ROOT "/rg", 200, 500);
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, args, &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("out.cap",
          !err && res != NULL && ends_with(res, "[grep output truncated]\n") &&
              agentc_strlen(res) <= AGENTC_LIMIT_TOOL_BYTES && ms < 10000);
    agentc_free(res);

    /* M1: a single very long line is stored as its 500-char prefix (charged
     * that way), so it fits the budget and yields path:line:<prefix>... with no
     * truncation marker -- exactly like the built-in. */
    write_rg_many_stub(ROOT "/rg", 1, 60000);
    {
        AgcBuf exp = { 0 };
        agentc_buf_cstr(&exp, "f.txt:1:");
        for (int i = 0; i < AGENTC_LIMIT_GREP_LINE; i++) agentc_buf_byte(&exp, 'x');
        agentc_buf_cstr(&exp, "...\n");
        agentc_buf_byte(&exp, 0);
        t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        res = run_external(&t, args, &err);
        ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
        check("longline.prefix",
              !err && res != NULL && agentc_streq(res, (const char *)exp.p) &&
                  agentc_str_str(res, "truncated") == NULL && ms < 10000);
        agentc_buf_free(&exp);
    }
    agentc_free(res);

    /* A final record without a trailing newline must still be processed. */
    write_stub(ROOT "/rg", STUB_RG_EOF);
    res = run_external(&t, args, &err);
    check("eof.flush.grep", !err && res != NULL && agentc_streq(res, "eof.txt:5:flushed\n"));
    agentc_free(res);

    /* Byte-budget sanity: a run whose emitted size and whose reader-side
     * accounting both stay under the cap is handed through whole -- every line
     * present, no truncation marker, no silently-trimmed tail. Each record is
     * charged path+text+32 (the worst-case formatting overhead), so 93 records
     * of 500 text bytes is the largest batch that fits: 93*(500+5+32) = 49941,
     * while the emitted text is 93*(500+5+4) = 47337 -- both just under the
     * 50000-byte cap. */
    enum { FIT_N = 93, FIT_TEXT = 500 };
    write_rg_many_stub(ROOT "/rg", FIT_N, FIT_TEXT);
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, args_fit, &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("fit.no-silent-trim",
          !err && res != NULL && count_byte(res, '\n') == (size_t)FIT_N &&
              agentc_strlen(res) == (size_t)FIT_N * (5 + 1 + 1 + 1 + FIT_TEXT + 1) &&
              agentc_str_str(res, "truncated") == NULL &&
              agentc_strlen(res) <= AGENTC_LIMIT_TOOL_BYTES && ms < 10000);
    agentc_free(res);

    /* 150 ms deadline on a spinning child: the driver reports the timeout and
     * the child (whose pid the stub recorded) must be gone afterwards. */
    os_unlink(ROOT "/busy.pid");
    write_stub(ROOT "/rg", STUB_BUSY);
    t.timeout_ms = 150;
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, args, &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("spawn.timeout", err && res != NULL &&
                               agentc_streq(res, "error: tool 'grep' timed out\n"));
    check("spawn.timeout.fast", ms < 10000);

    char pidbuf[32];
    int fd = os_open(ROOT "/busy.pid", OS_O_RDONLY, 0);
    int n = fd >= 0 ? os_read(fd, pidbuf, sizeof pidbuf - 1) : -1;
    if (fd >= 0) os_close(fd);
    if (n < 0) n = 0;
    pidbuf[n] = 0;
    while (n > 0 && (pidbuf[n - 1] == '\n' || pidbuf[n - 1] == '\r' || pidbuf[n - 1] == ' '))
        pidbuf[--n] = 0;
    bool parsed = false;
    i64 pid = agentc_parse_i64(pidbuf, agentc_strlen(pidbuf), &parsed);
    /* kill(pid, 0) fails only when the pid no longer names a live process */
    check("spawn.timeout.child-gone", parsed && pid > 0 && os_kill((int)pid, 0) != 0);
    agentc_free(res);

    /* G2: pinned POSIX grep with -Z. The probe stub exits 1, so init records
     * that -Z is supported, build_argv adds it and the reader switches to
     * NUL-delimited parsing. */
    write_stub(ROOT "/onlygrep/grep", STUB_GREP_NULL);
    write_stub(ROOT "/onlygrep/grep.exe", STUB_GREP_NULL);
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT "/onlygrep");
    agentc_memset(&sel, 0, sizeof sel);
    check("spawn.grep.null.select",
          agentc_tool_engine_select("grep", &sel) && sel.run != NULL &&
              agentc_streq(agentc_tool_engine_backend("grep"), AGENTC_ENGINE_GREP));
    agentc_memset(&t, 0, sizeof t);
    t.name = "grep";
    t.run = sel.run;
    res = run_external(&t, args, &err);
    check("spawn.grep.null",
          !err && res != NULL && agentc_streq(res, "report-2024-q1.txt:2:alpha here\n"));
    agentc_free(res);

    /* G2 fallback: the probe exits 2, so no -Z is passed and the textual parser
     * runs. The stub would exit 2 if -Z were present, so a clean match proves
     * the fallback omits it. */
    write_stub(ROOT "/onlygrep/grep", STUB_GREP_FALLBACK);
    write_stub(ROOT "/onlygrep/grep.exe", STUB_GREP_FALLBACK);
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT "/onlygrep");
    agentc_memset(&sel, 0, sizeof sel);
    (void)agentc_tool_engine_select("grep", &sel);
    agentc_memset(&t, 0, sizeof t);
    t.name = "grep";
    t.run = sel.run;
    res = run_external(&t, args, &err);
    check("spawn.grep.plain.fallback",
          !err && res != NULL && agentc_streq(res, "normal.txt:1:alpha\n"));
    agentc_free(res);

    /* D1: a grep whose usage error exits 1 must NOT be mistaken for -Z support.
     * The probe sees exit 1 but no NUL, falls back, and the textual record is
     * still found -- the old exit-code probe would have passed -Z and returned
     * "no matches" here. */
    write_stub(ROOT "/onlygrep/grep", STUB_GREP_USAGE_EXIT1);
    write_stub(ROOT "/onlygrep/grep.exe", STUB_GREP_USAGE_EXIT1);
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT "/onlygrep");
    agentc_memset(&sel, 0, sizeof sel);
    (void)agentc_tool_engine_select("grep", &sel);
    agentc_memset(&t, 0, sizeof t);
    t.name = "grep";
    t.run = sel.run;
    res = run_external(&t, args, &err);
    check("spawn.grep.usage-exit1",
          !err && res != NULL && agentc_streq(res, "normal.txt:1:alpha\n"));
    agentc_free(res);
}

/* find external rungs (POSIX only) through the real job driver. */
static void test_find_spawn(bool win) {
    const char *fargs = "{\"pattern\":\"*.txt\"}";
    const char *fargs_limit = "{\"pattern\":\"*.txt\",\"limit\":2}";

    if (win) {
        /* Documented skip (POSIX scripts do not exec under Windows); same label
         * values as the POSIX run so the shared golden stays identical. */
        check("spawn.find.select", true);
        check("spawn.find.sorted", true);
        check("spawn.find.dir-slash", true);
        check("spawn.find.none", true);
        check("spawn.find.limit", true);
        check("spawn.find.limit.stopped", true);
        check("spawn.find.error", true);
        check("spawn.find.fd.exit-nomatch", true);
        check("find.blank", true);
        check("find.out.cap", true);
        check("marker.fit", true);
        check("find.cancel", true);
        check("find.cancel.child-gone", true);
        check("eof.flush.find", true);
        check("spawn.find.posix.select", true);
        check("spawn.find.posix.sorted", true);
        check("spawn.find.posix.root-drop", true);
        check("spawn.find.posix.root-keep", true);
        check("spawn.find.posix.error", true);
        check("spawn.find.posix.exit-error", true);
        return;
    }

    AgcToolEngineSel sel;
    AgcTool t;
    bool err = false;
    char *res;
    i64 t0, ms;

    /* --- fd rung -------------------------------------------------------- */
    write_stub(ROOT "/fd", STUB_FD_MULTI);
    write_stub(ROOT "/fd.exe", STUB_FD_MULTI);
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT);
    check("spawn.find.select", agentc_tool_engine_select("find", &sel) && sel.run != NULL);
    agentc_memset(&t, 0, sizeof t);
    t.name = "find";
    t.run = sel.run;

    write_stub(ROOT "/fd", STUB_FD_MULTI);
    res = run_external(&t, fargs, &err);
    check("spawn.find.sorted",
          !err && res != NULL && agentc_streq(res, "a.txt\nb.txt\nc.txt\n"));
    agentc_free(res);

    /* G1: fd marks directories with a trailing '/', which must be stripped. */
    write_stub(ROOT "/fd", STUB_FD_DIRSLASH);
    res = run_external(&t, fargs, &err);
    check("spawn.find.dir-slash", !err && res != NULL && agentc_streq(res, "dir\nz.txt\n"));
    agentc_free(res);

    write_stub(ROOT "/fd", STUB_FD_EMPTY);
    res = run_external(&t, fargs, &err);
    check("spawn.find.none", !err && res != NULL && agentc_streq(res, "no matches\n"));
    agentc_free(res);

    write_stub(ROOT "/fd", STUB_FD_LIMIT);
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, fargs_limit, &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("spawn.find.limit",
          !err && res != NULL &&
              agentc_streq(res, "a.txt\nb.txt\n[find truncated at 2 paths]\n"));
    check("spawn.find.limit.stopped", ms < 10000);
    agentc_free(res);

    write_stub(ROOT "/fd", STUB_FD_ERROR);
    res = run_external(&t, fargs, &err);
    check("spawn.find.error", err && res != NULL && agentc_streq(res, "error: find: fd: boom\n"));
    agentc_free(res);

    /* fd exits 1 with no output when nothing matched -> success, no match */
    write_stub(ROOT "/fd", STUB_FD_EMPTY);
    res = run_external(&t, fargs, &err);
    check("spawn.find.fd.exit-nomatch",
          !err && res != NULL && agentc_streq(res, "no matches\n"));
    agentc_free(res);

    /* a "\r"-only stdout line must not become an empty path */
    write_stub(ROOT "/fd", STUB_FD_BLANK);
    res = run_external(&t, fargs, &err);
    check("find.blank", !err && res != NULL && agentc_streq(res, "a.txt\nb.txt\n"));
    agentc_free(res);

    /* find output-byte budget through the JOB driver: the engine reserves room for the
     * marker, so it is the one and only marker the driver sees and the result
     * stays within the display cap (no generic driver notice). */
    write_fd_big_stub(ROOT "/fd", (size_t)AGENTC_LIMIT_TOOL_BYTES + 10000);
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, fargs, &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("find.out.cap",
          !err && res != NULL && ends_with(res, "[find output truncated]\n") &&
              agentc_strlen(res) <= AGENTC_LIMIT_TOOL_BYTES && ms < 10000);
    agentc_free(res);

    /* Marker visibility through the JOB driver: 51 paths of 999 bytes. The
     * first 50 charge exactly 50*(999+1) = 50000 bytes; the 51st sets the count
     * limit, but the 29-byte marker no longer fits, so finish_output trims whole
     * lines until it does. The result ends with the marker, stays within the
     * cap and is strictly shorter than the untrimmed 50000. */
    enum { MARKER_PATHS = 51, MARKER_PATHLEN = 999, MARKER_LIMIT = 50 };
    write_fd_many_stub(ROOT "/fd", MARKER_PATHS, MARKER_PATHLEN);
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_external(&t, "{\"pattern\":\"*\",\"limit\":50}", &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("marker.fit",
          !err && res != NULL && ends_with(res, "[find truncated at 50 paths]\n") &&
              agentc_strlen(res) <= AGENTC_LIMIT_TOOL_BYTES &&
              agentc_strlen(res) < (size_t)MARKER_LIMIT * (MARKER_PATHLEN + 1) &&
              ms < 10000);
    agentc_free(res);

    /* Cancelled find: the partial path is kept, with no "no matches" fallback
     * and no truncation marker; the spinning child must be gone afterwards. */
    os_unlink(ROOT "/cancel.pid");
    write_stub(ROOT "/fd", STUB_FD_CANCEL);
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    res = run_cancel_external(&t, fargs, 300, &err);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("find.cancel",
          res != NULL && agentc_streq(res, "cancel.txt\n") &&
              agentc_str_str(res, "no matches") == NULL &&
              agentc_str_str(res, "[find") == NULL && ms < 10000);
    agentc_free(res);

    char cpidbuf[32];
    int cfd = os_open(ROOT "/cancel.pid", OS_O_RDONLY, 0);
    int cn = cfd >= 0 ? os_read(cfd, cpidbuf, sizeof cpidbuf - 1) : -1;
    if (cfd >= 0) os_close(cfd);
    if (cn < 0) cn = 0;
    cpidbuf[cn] = 0;
    while (cn > 0 &&
           (cpidbuf[cn - 1] == '\n' || cpidbuf[cn - 1] == '\r' || cpidbuf[cn - 1] == ' '))
        cpidbuf[--cn] = 0;
    bool cpid_ok = false;
    i64 cpid = agentc_parse_i64(cpidbuf, agentc_strlen(cpidbuf), &cpid_ok);
    check("find.cancel.child-gone", cpid_ok && cpid > 0 && os_kill((int)cpid, 0) != 0);

    /* A final path without a trailing newline must still be processed. */
    write_stub(ROOT "/fd", STUB_FD_EOF);
    res = run_external(&t, fargs, &err);
    check("eof.flush.find", !err && res != NULL && agentc_streq(res, "eof.txt\n"));
    agentc_free(res);

    /* --- POSIX find rung ------------------------------------------------ */
    write_stub(ROOT "/onlyfind/find", STUB_FIND_MULTI);
    write_stub(ROOT "/onlyfind/find.exe", STUB_FIND_MULTI);
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT "/onlyfind");
    check("spawn.find.posix.select", agentc_tool_engine_select("find", &sel) && sel.run != NULL);
    agentc_memset(&t, 0, sizeof t);
    t.name = "find";
    t.run = sel.run;

    write_stub(ROOT "/onlyfind/find", STUB_FIND_MULTI);
    res = run_external(&t, fargs, &err);
    check("spawn.find.posix.sorted", !err && res != NULL && agentc_streq(res, "y.txt\nz.txt\n"));
    agentc_free(res);

    /* G1: POSIX find echoes a directory search root; it must be dropped while
     * the child is kept. A file root is not a directory and must be kept. */
    write_stub(ROOT "/onlyfind/find", STUB_FIND_ROOT);
    res = run_external(&t, "{\"pattern\":\"*\",\"path\":\"" ROOT "\"}", &err);
    check("spawn.find.posix.root-drop",
          !err && res != NULL && agentc_streq(res, ROOT "/child.txt\n"));
    agentc_free(res);

    write_stub(ROOT "/rootfile", "#!/bin/sh\nexit 0\n");
    write_stub(ROOT "/onlyfind/find", STUB_FIND_OPERAND);
    res = run_external(&t, "{\"pattern\":\"*\",\"path\":\"" ROOT "/rootfile\"}", &err);
    check("spawn.find.posix.root-keep",
          !err && res != NULL && agentc_streq(res, ROOT "/rootfile\n"));
    agentc_free(res);

    write_stub(ROOT "/onlyfind/find", STUB_FIND_ERROR);
    res = run_external(&t, fargs, &err);
    check("spawn.find.posix.error",
          err && res != NULL && agentc_streq(res, "error: find: find: bang\n"));
    agentc_free(res);

    /* POSIX find exiting 1 is a real error, not "no matches" */
    write_stub(ROOT "/onlyfind/find", STUB_FIND_EXIT1_ERR);
    res = run_external(&t, fargs, &err);
    check("spawn.find.posix.exit-error",
          err && res != NULL && agentc_streq(res, "error: find: find: boom\n"));
    agentc_free(res);
}

/* The shared parsers reject an empty pattern with the built-in error text
 * before any spawn, on every platform (so this needs no POSIX gate). The stub
 * sentinel proves no child ran. */
static void test_arg_empty(void) {
    struct os_stat st;

    os_unlink(ROOT "/ran-rg");
    os_unlink(ROOT "/ran-fd");
    write_stub(ROOT "/rg", STUB_MARK_RG);
    write_stub(ROOT "/fd", STUB_MARK_FD);
    agentc_tool_engine_reset();
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_EXTERNAL, ROOT);

    AgcToolEngineSel sel;
    AgcTool t;
    bool err = false;
    char *res;

    agentc_memset(&sel, 0, sizeof sel);
    (void)agentc_tool_engine_select("grep", &sel);
    agentc_memset(&t, 0, sizeof t);
    t.name = "grep";
    t.run = sel.run;
    res = run_external(&t, "{\"pattern\":\"\"}", &err);
    check("arg.empty.grep",
          err && res != NULL && agentc_streq(res, "error: grep: pattern is required") &&
              os_stat(ROOT "/ran-rg", &st) != 0);
    agentc_free(res);

    agentc_memset(&sel, 0, sizeof sel);
    (void)agentc_tool_engine_select("find", &sel);
    agentc_memset(&t, 0, sizeof t);
    t.name = "find";
    t.run = sel.run;
    res = run_external(&t, "{\"pattern\":\"\"}", &err);
    check("arg.empty.find",
          err && res != NULL && agentc_streq(res, "error: find: pattern is required") &&
              os_stat(ROOT "/ran-fd", &st) != 0);
    agentc_free(res);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    agentc_rm_rf(ROOT);
    (void)os_mkdir(ROOT, 0755);
    (void)os_mkdir(ROOT "/onlyfd", 0755);
    (void)os_mkdir(ROOT "/onlyfind", 0755);
    (void)os_mkdir(ROOT "/onlygrep", 0755);
    bool win = agentc_streq(os_platform(), "windows");

    test_parse();
    test_which(win);
    test_selection();
    test_find_selection(win);
    test_desc();
    test_argv();
    test_rg_line();
    test_grep_line();
    test_hits();
    test_spawn(win);
    test_find_spawn(win);
    test_arg_empty();

    /* restore a clean process-wide engine state for any later test in-process */
    agentc_tool_engine_reset();
    agentc_rm_rf(ROOT);
    return fails;
}
