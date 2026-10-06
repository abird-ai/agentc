/* tools2_test.c — ls/find/grep against a tree under /tmp: sorting, "/" suffix,
 * glob matching, .gitignore, symlinked directories, context and line caps.
 */
#include "agent.h"
#include "plat.h"
#include "base/glob.h"

/* internal helpers (not in the frozen headers) */
char *agentc_tool_ls(const char *path, i64 limit, bool *is_error);
char *agentc_tool_find(const char *pattern, const char *path, i64 limit, bool *is_error);
char *agentc_tool_grep(const char *pattern, const char *path, const char *glob,
                   bool ignore_case, bool literal, i64 context, i64 limit,
                   bool *is_error);
void agentc_rm_rf(const char *path);

#define ROOT "/tmp/agentc-tools2-test"
#define SROOT "/tmp/agentc-rm-symlink-root"
#define SOUT "/tmp/agentc-rm-symlink-outside"

static int fails;
static bool g_symlink_ready;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

/* Call a builtin's v2 sync entry point and turn its buffer into the owned
 * string the old exec call sites returned. */
static char *run_tool(const AgcTool *t, const char *args, bool *is_error) {
    AgcToolCall call = { "test", t->name, args, NULL };
    AgcBuf out = { 0 };
    bool err = false;
    (void)t->run(t, &call, &out, &err);
    agentc_buf_byte(&out, 0);
    out.len--;
    if (is_error) *is_error = err;
    if (out.p == NULL) return agentc_strdup_len("", 0);
    return (char *)out.p;
}

static void write_file(const char *path, const char *content) {
    bool err = false;
    char *res = agentc_tool_write(path, content, &err);
    agentc_free(res);
}

static void make_long_line(char *out, size_t cap) {
    size_t o = 0;
    const char *head = "beta ";
    while (o + 6 < cap) {
        agentc_memcpy(out + o, head, 5);
        o += 5;
    }
    agentc_memcpy(out + o, "ZZZEND", 6);
    o += 6;
    out[o] = 0;
}

static void setup_tree(void) {
    agentc_rm_rf(ROOT);
    (void)os_mkdir(ROOT, 0755);
    write_file(ROOT "/a.txt", "alpha\nbeta\ngamma\n");
    write_file(ROOT "/b.c", "int beta = 1;\n");
    write_file(ROOT "/sub/x.c", "int subvar = 2;\n");
    write_file(ROOT "/sub/keep.txt", "beta keep\n");
    write_file(ROOT "/sub/deep/d.md", "no match here\n");
    write_file(ROOT "/ignored.txt", "beta ignored\n");
    write_file(ROOT "/ignored_dir/y.txt", "beta ignored dir\n");
    write_file(ROOT "/a.log", "not beta\n");
    write_file(ROOT "/keep.log", "not beta\n");
    write_file(ROOT "/.gitignore", "ignored.txt\nignored_dir/\n*.log\n!keep.log\n");
    write_file(ROOT "/.git/config", "beta in git\n");
    write_file(ROOT "/.hidden", "beta hidden\n");
    char longline[1200];
    make_long_line(longline, sizeof longline);
    write_file(ROOT "/long.txt", longline);

    /* symlink a directory: find/grep must list it but never recurse.
     * Windows has no `ln -s`; a directory junction (`mklink /J`) needs no
     * privilege and is a reparse point, which is exactly what find/grep must
     * list and not follow. Wine's cmd does not implement mklink, so there the
     * checks stay gated on g_symlink_ready. The shared golden file has no skip
     * note, so the note is POSIX-only. `rm -f` first: Wine cannot delete a Unix
     * symlink-to-directory through DeleteFileW/RemoveDirectoryW (both follow
     * the reparse point), so a stale link from an earlier run would make `ln`
     * print to the test's own stderr. Wine's Unix-loader child has no waitable
     * handle either, so poll briefly for the side effect before deciding. */
    bool err = false;
    bool win = agentc_streq(os_platform(), "windows");
    const char *cmdstr = win ? "mklink /J \\tmp\\agentc-tools2-test\\linkdir "
                               "\\tmp\\agentc-tools2-test"
                             : "rm -f " ROOT "/linkdir; ln -s . " ROOT "/linkdir";
    char *cmd = agentc_tool_bash(cmdstr, 0, NULL, &err);
    struct os_stat st;
    for (int i = 0; i < 50 && os_stat(ROOT "/linkdir", &st) < 0; i++)
        os_sleep_ns(10 * 1000000);
    g_symlink_ready = os_stat(ROOT "/linkdir", &st) == 0;
    if (!g_symlink_ready && !win)
        agentc_outf("note: symlink creation failed; find/grep symlink checks skipped (%s)\n",
                    cmd && cmd[0] ? cmd : "no output");
    agentc_free(cmd);
}

static void test_registry(void) {
    AgcTool tools[8];
    size_t n = agentc_tools_builtin(tools, 8);
    check("reg_count", n == 7);
    bool names = n >= 7 && agentc_streq(tools[4].name, "ls") && agentc_streq(tools[5].name, "find") &&
                 agentc_streq(tools[6].name, "grep");
    check("reg_names", names);

    bool err = false;
    char *res = run_tool(&tools[5], "{}", &err);
    check("find_missing_pattern", err && contains(res, "missing required field: pattern"));
    agentc_free(res);
    res = run_tool(&tools[6], "{}", &err);
    check("grep_missing_pattern", err && contains(res, "missing required field: pattern"));
    agentc_free(res);
    res = run_tool(&tools[4], "[]", &err);
    check("ls_bad_root", err && contains(res, "must be a JSON object"));
    agentc_free(res);
}

static void test_ls(void) {
    bool err = false;
    char *res = agentc_tool_ls(ROOT, 0, &err);
    check("ls_ok", !err && res);
    check("ls_dir_suffix", contains(res, "sub/\n") && contains(res, "ignored_dir/\n"));
    check("ls_files", contains(res, "a.txt\n") && contains(res, ".hidden\n") &&
                          contains(res, ".gitignore\n"));
    check("ls_sorted", res && agentc_str_str(res, "a.txt\n") && agentc_str_str(res, "b.c\n") &&
                           agentc_str_str(res, "a.txt\n") < agentc_str_str(res, "b.c\n"));
    agentc_free(res);

    res = agentc_tool_ls(ROOT, 2, &err);
    check("ls_limit", !err && contains(res, "[listing truncated: showing 2 of "));
    agentc_free(res);

    res = agentc_tool_ls(ROOT "/a.txt", 0, &err);
    check("ls_not_dir", err && contains(res, "not a directory"));
    agentc_free(res);
}

static void test_find(void) {
    bool err = false;
    char *res = agentc_tool_find("*.txt", ROOT, 0, &err);
    check("find_txt", !err && contains(res, ROOT "/a.txt") &&
                          contains(res, ROOT "/sub/keep.txt") &&
                          contains(res, ROOT "/long.txt"));
    check("find_gitignore", !contains(res, "ignored.txt") &&
                                !contains(res, "ignored_dir"));
    check("find_no_git", !contains(res, "/.git/"));
    agentc_free(res);

    res = agentc_tool_find("**/*.c", ROOT, 0, &err);
    check("find_dstar", !err && contains(res, ROOT "/b.c") && contains(res, ROOT "/sub/x.c"));
    agentc_free(res);

    res = agentc_tool_find("*", ROOT, 0, &err);
    check("find_symlink_listed", !g_symlink_ready || contains(res, ROOT "/linkdir"));
    check("find_symlink_not_followed", !g_symlink_ready || !contains(res, "linkdir/"));
    agentc_free(res);

    res = agentc_tool_find("*.txt", ROOT, 2, &err);
    check("find_limit", !err && contains(res, "[find truncated at 2 paths]"));
    agentc_free(res);

    res = agentc_tool_find("*.nomatch", ROOT, 0, &err);
    check("find_none", !err && agentc_streq(res, "no matches\n"));
    agentc_free(res);

    /* `!pattern` re-includes: a later negation wins over an earlier `*.log`. */
    res = agentc_tool_find("*.log", ROOT, 0, &err);
    check("find_gitignore_negation_keep", !err && contains(res, ROOT "/keep.log"));
    check("find_gitignore_negation_skip", !err && !contains(res, "a.log"));
    agentc_free(res);
}

static void test_grep(void) {
    bool err = false;
    char *res = agentc_tool_grep("beta", ROOT, NULL, false, true, 0, 0, &err);
    check("grep_literal", !err && contains(res, ROOT "/a.txt:2:beta") &&
                              contains(res, ROOT "/b.c:1:int beta = 1;"));
    check("grep_gitignore", !contains(res, "ignored.txt") &&
                                !contains(res, "ignored_dir") && !contains(res, "/.git/"));
    check("grep_symlink_not_followed", !g_symlink_ready || !contains(res, "linkdir/"));
    agentc_free(res);

    res = agentc_tool_grep("^int [a-z]+", ROOT, "*.c", false, false, 0, 0, &err);
    check("grep_regex", !err && contains(res, ROOT "/b.c:1:int beta = 1;") &&
                            contains(res, ROOT "/sub/x.c:1:int subvar = 2;"));
    agentc_free(res);

    res = agentc_tool_grep("BETA", ROOT, "a.txt", true, false, 0, 0, &err);
    check("grep_icase", !err && contains(res, ROOT "/a.txt:2:beta"));
    agentc_free(res);

    res = agentc_tool_grep("beta", ROOT "/a.txt", NULL, false, true, 1, 0, &err);
    check("grep_context", !err && contains(res, ROOT "/a.txt:1:alpha") &&
                              contains(res, ROOT "/a.txt:2:beta") &&
                              contains(res, ROOT "/a.txt:3:gamma"));
    agentc_free(res);

    res = agentc_tool_grep("beta", ROOT, "*.txt", false, true, 0, 0, &err);
    check("grep_glob", !err && contains(res, ROOT "/a.txt:2:beta") &&
                           !contains(res, "b.c"));
    agentc_free(res);

    res = agentc_tool_grep("beta", ROOT, NULL, false, true, 0, 1, &err);
    check("grep_limit", !err && contains(res, "[grep truncated at 1 matches]"));
    agentc_free(res);

    res = agentc_tool_grep("beta", ROOT "/long.txt", NULL, false, true, 0, 0, &err);
    check("grep_line_cap", !err && contains(res, "...") && !contains(res, "ZZZEND") &&
                                agentc_str_str(res, "beta beta beta") != NULL);
    agentc_free(res);

    res = agentc_tool_grep("nomatchxyz", ROOT, NULL, false, true, 0, 0, &err);
    check("grep_none", !err && agentc_streq(res, "no matches\n"));
    agentc_free(res);

    res = agentc_tool_grep("beta", ROOT "/nope", NULL, false, true, 0, 0, &err);
    check("grep_missing", err && contains(res, "cannot access"));
    agentc_free(res);
}

/* The bounded matcher: over-long patterns and over-budget matches fail fast
 * instead of hanging (see base/glob.h). */
static void test_glob_bounds(void) {
    char cap[300];
    agentc_memset(cap, 'a', sizeof cap - 1);
    cap[sizeof cap - 1] = 0;
    check("glob.cap", !agentc_glob_match(cap, "a"));

    /* 15 stars: exponential for a recursive matcher against a long run of a's */
    const char *pattern = "*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a";
    char *text = agentc_alloc(200000 + 1);
    agentc_memset(text, 'a', 200000);
    text[200000] = 0;
    check("glob.budget", !agentc_glob_match(pattern, text));
    agentc_free(text);
}

/* rm_rf must unlink a symlink, never descend into its target. The link is made
 * with `ln -s` (agentc has no portable os_symlink) and the case is POSIX-only. */
static void test_rm_rf_symlink(void) {
    if (agentc_streq(os_platform(), "windows")) {
        check("rm_symlink_target_survives", true); /* documented skip: junctions */
        return;
    }
    agentc_rm_rf(SROOT);
    agentc_rm_rf(SOUT);
    (void)os_mkdir(SOUT, 0755);
    (void)os_mkdir(SROOT, 0755);
    write_file(SOUT "/sentinel.txt", "keep me\n");
    write_file(SROOT "/inside.txt", "remove me\n");

    bool err = false;
    char *cmd = agentc_tool_bash("ln -s " SOUT " " SROOT "/linkdir", 0, NULL, &err);
    agentc_free(cmd);
    struct os_stat st;
    if (os_stat(SROOT "/linkdir", &st) != 0) {
        check("rm_symlink_target_survives", true); /* no symlink support here */
    } else {
        agentc_rm_rf(SROOT);
        check("rm_symlink_target_survives", os_stat(SOUT "/sentinel.txt", &st) == 0);
    }
    os_unlink(SOUT "/sentinel.txt");
    os_unlink(SROOT "/inside.txt");
    agentc_rm_rf(SOUT);
    agentc_rm_rf(SROOT);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setup_tree();
    test_registry();
    test_ls();
    test_find();
    test_grep();
    test_glob_bounds();
    test_rm_rf_symlink();
    agentc_rm_rf(ROOT);
    return fails;
}
