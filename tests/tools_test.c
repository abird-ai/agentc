/* tools_test.c — read/write/edit/bash against files under /tmp. */
#include "agent.h"
#include "config.h"
#include "plat.h"
#include "core/tools/jobs.h"

#define TDIR "/tmp/agentc-tools-test"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool contains(const char *hay, const char *needle) {
    return hay && agentc_str_str(hay, needle) != NULL;
}

/* Byte-exact whole-file check, for the NUL-preserving write paths. */
static bool raw_is(const char *path, const void *want, size_t n) {
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return false;
    char buf[64];
    size_t got = 0;
    while (got < sizeof buf) {
        int r = os_read(fd, buf + got, sizeof buf - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    os_close(fd);
    return got == n && agentc_memeq(buf, want, n);
}

/* v2 registry call: run() directly when the builtin has one; the async bash is
 * driven through the production job driver (start/step plus its cleanup). */
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

static char *run_tool(const AgcTool *t, const char *args, bool *is_error) {
    AgcToolCall call = { "test", t->name, args, NULL };
    AgcBuf out = { 0 };
    bool err = false;
    if (t->run != NULL) {
        (void)t->run(t, &call, &out, &err);
    } else {
        RunCtx c;
        agentc_memset(&c, 0, sizeof c);
        const AgcTool *tools[1] = { t };
        (void)agentc_tool_jobs_run(tools, NULL, &call, 1, run_collect, &c);
        out = c.out;
        err = c.is_error;
    }
    agentc_buf_byte(&out, 0);
    out.len--;
    if (is_error) *is_error = err;
    if (out.p == NULL) return agentc_strdup_len("", 0);
    return (char *)out.p;
}

/* Same, but the async pair is driven by hand so the cancel flag can flip after
 * start(); this deterministically exercises bash's own [aborted] text. */
static char *run_tool_cancel(const AgcTool *t, const char *args, volatile bool *cancel,
                             bool *is_error) {
    AgcToolCall call = { "test", t->name, args, cancel };
    AgcBuf out = { 0 };
    AgcJob job;
    agentc_tool_job_init(&job);
    job.tool = t;
    job.call = call;
    int r = t->start(t, &call, &job);
    if (r == 0) {
        *cancel = true;
        while ((r = t->step(t, &job)) == 0) { }
    }
    if (job.out.len > 0) agentc_buf_push(&out, job.out.p, job.out.len);
    if (is_error) *is_error = job.is_error;
    agentc_free(job.priv);
    if (job.fd >= 0) os_close(job.fd);
    if (job.spill_fd >= 0) os_close(job.spill_fd);
    agentc_buf_free(&job.out);
    agentc_buf_free(&job.spill_path);
    agentc_buf_byte(&out, 0);
    out.len--;
    if (out.p == NULL) return agentc_strdup_len("", 0);
    return (char *)out.p;
}

static char *make_lines(size_t nlines, const char *prefix) {
    AgcBuf b = { 0 };
    for (size_t i = 0; i < nlines; i++)
        agentc_buf_printf(&b, "%s %llu\n", prefix, (unsigned long long)(i + 1));
    return (char *)b.p;
}

static void test_write_read(void) {
    bool err = false;
    char *res = agentc_tool_write(TDIR "/a.txt", "hello\nworld\n", &err);
    check("write_ok", !err && res && contains(res, "wrote 12 bytes"));
    agentc_free(res);

    res = agentc_tool_read(TDIR "/a.txt", 0, 0, &err);
    check("read_small", !err && res && agentc_streq(res, "hello\nworld\n"));
    agentc_free(res);

    res = agentc_tool_write(TDIR "/nested/deep/b.txt", "nested\n", &err);
    check("write_nested", !err && res && contains(res, "wrote 7 bytes"));
    agentc_free(res);
    res = agentc_tool_read(TDIR "/nested/deep/b.txt", 0, 0, &err);
    check("read_nested", !err && res && agentc_streq(res, "nested\n"));
    agentc_free(res);

    res = agentc_tool_read(TDIR "/nope.txt", 0, 0, &err);
    check("read_missing_err", err && res && contains(res, "cannot open"));
    agentc_free(res);

    /* binary sniff: a single NUL rejects the whole file */
    int bfd = os_open(TDIR "/binary.dat", OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (bfd >= 0) {
        const char bin[] = { 'a', 0, 'b' };
        (void)os_write(bfd, bin, sizeof bin);
        os_close(bfd);
    }
    res = agentc_tool_read(TDIR "/binary.dat", 0, 0, &err);
    check("read_binary_err", err && res && contains(res, "binary file"));
    agentc_free(res);
    os_unlink(TDIR "/binary.dat");
}

/* The atomic temp+rename replace must keep the target's mode: overwriting a
 * 0600 file must not widen it to the 0644 default. Windows synthesises
 * st_mode, so the assertion is POSIX-only. */
static void test_write_mode(void) {
    const char *path = TDIR "/secret.txt";
    os_unlink(path);
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0600);
    if (fd >= 0) {
        (void)os_write(fd, "old\n", 4);
        os_close(fd);
    }
    bool err = false;
    char *res = agentc_tool_write(path, "new\n", &err);
    agentc_free(res);
    bool ok = !err;
    if (ok && !agentc_streq(os_platform(), "windows")) {
        struct os_stat st;
        ok = os_stat(path, &st) == 0 && (st.st_mode & 0777) == 0600;
    }
    check("write_mode_preserved", ok);
    os_unlink(path);
}

/* The explicit-length write form must preserve bytes that a strlen copy would
 * cut at the first NUL, through the public helper, the registry and edit. */
static void test_write_len(void) {
    bool err = false;
    const char direct[] = { 'a', 0, 'b', '\n' };
    char *res = agentc_tool_write_len(TDIR "/nul.dat", direct, sizeof direct, &err);
    check("write_len_ok", !err && res && contains(res, "wrote 4 bytes"));
    agentc_free(res);
    check("write_len_nul", raw_is(TDIR "/nul.dat", direct, sizeof direct));

    AgcTool tools[8];
    (void)agentc_tools_builtin(tools, 8);

    /* registry write_run: the JSON string's own length, not strlen */
    res = run_tool(&tools[3], "{\"path\":\"" TDIR "/nul2.dat\",\"content\":\"a\\u0000b\"}",
                   &err);
    check("write_len_registry", !err && res && contains(res, "wrote 3 bytes") &&
                                     raw_is(TDIR "/nul2.dat", "a\0b", 3));
    agentc_free(res);

    /* edit composes the replacement in memory; the final write must keep NUL */
    res = run_tool(&tools[3], "{\"path\":\"" TDIR "/nul3.dat\",\"content\":\"one\\ntwo\\n\"}",
                   &err);
    agentc_free(res);
    const char want[] = { 'o', 'n', 'e', '\n', 'T', 0, 'O', '\n' };
    res = run_tool(&tools[2],
                   "{\"path\":\"" TDIR "/nul3.dat\",\"edits\":[{\"oldText\":\"two\\n\","
                   "\"newText\":\"T\\u0000O\\n\"}]}",
                   &err);
    check("write_len_edit", !err && raw_is(TDIR "/nul3.dat", want, sizeof want));
    agentc_free(res);

    os_unlink(TDIR "/nul.dat");
    os_unlink(TDIR "/nul2.dat");
    os_unlink(TDIR "/nul3.dat");
}

static void test_read_truncation(void) {
    bool err = false;
    char *big = make_lines(2500, "line");
    char *res = agentc_tool_write(TDIR "/big.txt", big, &err);
    agentc_free(big);
    check("write_big", !err);
    agentc_free(res);

    res = agentc_tool_read(TDIR "/big.txt", 0, 0, &err);
    check("read_trunc_notice",
          !err && res &&
              contains(res, "[Showing lines 1-2000 of 2500. Use offset=2000 to continue.]"));
    check("read_trunc_first", contains(res, "line 1\n") && contains(res, "line 2000\n"));
    check("read_trunc_stops", !contains(res, "line 2001"));
    agentc_free(res);

    res = agentc_tool_read(TDIR "/big.txt", 2000, 0, &err);
    check("read_offset",
          !err && res && contains(res, "line 2001") && contains(res, "line 2500") &&
              !contains(res, "Showing lines"));
    agentc_free(res);
}

static void test_read_limits(void) {
    bool err = false;
    /* The 64 MiB guard is a size check, so build a sparse >64 MiB file with
     * lseek: this exercises the same code on every platform. /dev/zero has no
     * Windows counterpart (NUL reports size 0 and EOFs). */
    int fd = os_open(TDIR "/huge.txt", OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (fd >= 0) {
        (void)os_lseek(fd, (64 * 1024 * 1024) + 1, 0 /* SEEK_SET */);
        (void)os_write(fd, "x", 1);
        os_close(fd);
    }
    char *res = agentc_tool_read(TDIR "/huge.txt", 0, 0, &err);
    bool huge = err && res && contains(res, "larger than");
    agentc_free(res);

    /* POSIX /dev/zero has st_size 0 yet never reaches EOF; the streaming guard
     * must stop that too. Windows maps /dev/zero to NUL, which EOFs, so only
     * the size guard above is meaningful there. */
    bool zero = true;
    if (!agentc_streq(os_platform(), "windows")) {
        res = agentc_tool_read("/dev/zero", 0, 0, &err);
        zero = err && res && contains(res, "larger than");
        agentc_free(res);
    }
    os_unlink(TDIR "/huge.txt");
    check("read_devzero", huge && zero);
}

static void test_edit(void) {
    bool err = false;
    char *res = agentc_tool_write(TDIR "/multi.txt", "alpha\nbeta\ngamma\n", &err);
    agentc_free(res);

    res = agentc_tool_edit(TDIR "/multi.txt",
                       "[{\"oldText\":\"beta\\n\",\"newText\":\"BETA\\n\"}]", &err);
    check("edit_ok", !err && res && contains(res, "1 edit(s)") &&
                         contains(res, "@@ -2,1 +2,1 @@") && contains(res, "-beta\n") &&
                         contains(res, "+BETA\n"));
    agentc_free(res);

    res = agentc_tool_read(TDIR "/multi.txt", 0, 0, &err);
    check("edit_applied", !err && res && agentc_streq(res, "alpha\nBETA\ngamma\n"));
    agentc_free(res);

    /* duplicate match: rejected, file unchanged */
    res = agentc_tool_write(TDIR "/dup.txt", "aaaa\n", &err);
    agentc_free(res);
    res = agentc_tool_edit(TDIR "/dup.txt", "[{\"oldText\":\"aa\",\"newText\":\"b\"}]", &err);
    check("edit_dup_err", err && res && contains(res, "not unique"));
    agentc_free(res);
    res = agentc_tool_read(TDIR "/dup.txt", 0, 0, &err);
    check("edit_dup_unchanged", !err && res && agentc_streq(res, "aaaa\n"));
    agentc_free(res);

    res = agentc_tool_edit(TDIR "/dup.txt", "[{\"oldText\":\"zz\",\"newText\":\"b\"}]", &err);
    check("edit_missing_err", err && res && contains(res, "not found"));
    agentc_free(res);

    /* CRLF + BOM survive an edit */
    res = agentc_tool_write(TDIR "/crlf.txt", "\xEF\xBB\xBFone\r\ntwo\r\n", &err);
    agentc_free(res);
    res = agentc_tool_edit(TDIR "/crlf.txt",
                       "[{\"oldText\":\"two\\n\",\"newText\":\"TWO\\n\"}]", &err);
    check("edit_crlf_ok", !err);
    agentc_free(res);
    int fd = os_open(TDIR "/crlf.txt", OS_O_RDONLY, 0);
    char raw[256];
    int n = fd >= 0 ? os_read(fd, raw, sizeof raw) : -1;
    if (fd >= 0) os_close(fd);
    check("edit_bom", n >= 3 && (u8)raw[0] == 0xEF && (u8)raw[1] == 0xBB && (u8)raw[2] == 0xBF);
    if (n < 0) n = 0;
    raw[n] = 0;
    check("edit_crlf_preserved", agentc_str_str(raw, "one\r\nTWO\r\n") != NULL);
}

/* On Windows the bash tool now runs cmd.exe or PowerShell (selected by the
 * `shell` config key; the Windows CI runner picks one per run with
 * AGENTC_SHELL, and the tool itself honours that override). The tests do not
 * load config files, so apply the override here and choose commands that the
 * selected shell actually understands. POSIX keeps the /bin/sh assertions
 * byte-for-byte. */
static const char *selected_shell(void) {
    const char *env = os_getenv("AGENTC_SHELL");
    if (env && env[0]) agentc_config_set_shell(env);
    return agentc_config_shell();
}

static bool shell_is(const char *kind, const char *want) {
    const char *k = os_shell_kind(kind);
    return k != NULL && agentc_streq(k, want);
}

/* TDIR with Windows separators: cmd.exe and PowerShell read "/tmp/x" as a
 * switch, not a path, so Windows commands use backslashes. */
static const char *win_tdir(void) {
    static char buf[64];
    if (buf[0]) return buf;
    const char *s = TDIR;
    size_t i = 0;
    while (s[i] != 0 && i + 1 < sizeof buf) {
        buf[i] = s[i] == '/' ? '\\' : s[i];
        i++;
    }
    buf[i] = 0;
    return buf;
}

/* Echo through a redirected file: one check proves output capture, exit code
 * zero and redirection at once, without assuming either shell's quoting. */
static char *bash_ok_command(const char *kind) {
    AgcBuf b = { 0 };
    if (shell_is(kind, "cmd"))
        agentc_buf_printf(&b,
                          "echo one> \"%s\\shell.txt\" & type \"%s\\shell.txt\" "
                          "& echo two",
                          win_tdir(), win_tdir());
    else if (shell_is(kind, "powershell") || shell_is(kind, "pwsh"))
        agentc_buf_printf(&b,
                          "'one' > '%s\\shell.txt'; Get-Content '%s\\shell.txt'; 'two'",
                          win_tdir(), win_tdir());
    else
        agentc_buf_cstr(&b, "printf 'one\\ntwo\\n'");
    return (char *)b.p;
}

/* The other Windows shell: real execution when it is installed (the CI runner
 * has both) and the actionable missing-shell error when it is not. `pwsh` is
 * the strict PowerShell 7 name, so Wine (which ships an inert `powershell.exe`
 * stub but no pwsh.exe) really does take the missing-shell path here instead
 * of silently running a no-op. Restores the selected kind afterwards. */
static bool bash_alt_shell_ok(const char *kind) {
    const char *alt =
        (shell_is(kind, "powershell") || shell_is(kind, "pwsh")) ? "cmd" : "pwsh";
    char path[1024];
    bool present = os_shell_resolve(alt, path, sizeof path) == 0;
    char saved[64];
    agentc_snprintf(saved, sizeof saved, "%s", agentc_config_shell());
    agentc_config_set_shell(alt);
    bool err = false;
    char *res = agentc_tool_bash("echo hi", 0, NULL, &err);
    bool ok;
    if (present)
        ok = !err && res && contains(res, "hi") && contains(res, "exit code: 0");
    else
        ok = err && res && contains(res, "not found");
    agentc_free(res);
    agentc_config_set_shell(saved);
    return ok;
}

/* A child that closes its stdout and lives on (`exec 1>&-; sleep ...`) must
 * hit the deadline. The both-fds-closed form makes the read end EOF while the
 * child is still alive, which must keep polling until the deadline instead of
 * falling into a blocking reap. Windows cmd/PowerShell have no `exec`, so a
 * never-exiting equivalent checks the same no-hang guarantee. */
static void test_bash_eof_hang(const char *kind) {
    bool ok = true;
    if (shell_is(kind, "sh")) {
        const char *cmds[] = {
            "exec 1>&-; sleep 100000",      /* stderr still holds the pipe write end */
            "exec 1>&- 2>&-; sleep 100000", /* EOF on the read end, child alive */
        };
        for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
            bool err = false;
            i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
            char *res = agentc_tool_bash(cmds[i], 1500, NULL, &err);
            i64 ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
            if (!(err && res && contains(res, "[timed out") && ms < 10000)) ok = false;
            agentc_free(res);
        }
    } else {
        const char *cmd = shell_is(kind, "cmd")
                              ? "for /l %i in (1,1,2000000000) do @rem"
                              : "while ($true) {}";
        bool err = false;
        i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        char *res = agentc_tool_bash(cmd, 1500, NULL, &err);
        i64 ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
        ok = err && res && contains(res, "[timed out") && ms < 10000;
        agentc_free(res);
    }
    check("bash.eof-hang", ok);
}

/* Cancelling must kill the whole process group, not just the shell. The
 * background subshell ignores SIGTERM (and its sleep inherits SIG_IGN), so a
 * leader-only kill leaves it alive to write the sentinel file at ~4 s; only the
 * group SIGKILL escalation stops it. cmd/PowerShell cannot express the trap, so
 * Windows counts this as a documented skip (bash.eof-hang still proves the
 * no-hang guarantee there). */
static void test_bash_group_kill(bool win) {
    if (win) {
        check("bash.group-kill", true); /* documented skip: no POSIX trap */
        return;
    }
    char path[128];
    agentc_snprintf(path, sizeof path, "/tmp/agentc-bash-group-%llx",
                    (unsigned long long)(os_now_ns(OS_CLOCK_MONOTONIC) & 0xffffff));
    os_unlink(path);
    char cmd[512];
    agentc_snprintf(cmd, sizeof cmd,
                    "(trap '' TERM; sleep 4; echo alive > %s) & echo started; wait", path);

    bool ok = false;
    AgcTool tools[8];
    size_t n = agentc_tools_builtin(tools, 8);
    const AgcTool *bash = n > 1 ? &tools[1] : NULL;
    if (bash != NULL && bash->start != NULL) {
        char args[640];
        agentc_snprintf(args, sizeof args, "{\"command\":\"%s\"}", cmd);
        volatile bool cancel = false;
        AgcToolCall call = { "test", "bash", args, &cancel };
        AgcJob job;
        agentc_tool_job_init(&job);
        job.tool = bash;
        job.call = call;
        int pid = -1;
        int r = bash->start(bash, &call, &job);
        pid = job.pid;
        i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        while (r == 0 && (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000 < 200) {
            r = bash->step(bash, &job);
            if (r == 0) os_poll(NULL, 0, 5);
        }
        cancel = true;
        while (r == 0) r = bash->step(bash, &job);

        bool tree_dead = false;
        if (pid > 0) {
            os_sleep_ns(1000000000); /* let a surviving grandchild write it */
            struct os_stat st;
            if (os_stat(path, &st) != 0) {
                /* The group must be empty as well: a leader-only kill leaves
                 * the TERM-ignoring subshell (and its sleep) running. */
                tree_dead = os_kill(-pid, 0) != 0;
            }
        }
        ok = r == 1 && tree_dead;
        agentc_free(job.priv);
        if (job.fd >= 0) os_close(job.fd);
        if (job.spill_fd >= 0) os_close(job.spill_fd);
        agentc_buf_free(&job.out);
        agentc_buf_free(&job.spill_path);
    }
    os_unlink(path);
    check("bash.group-kill", ok);
}

static void test_bash(void) {
    bool err = false;
    const char *kind = selected_shell();
    bool win = agentc_streq(os_platform(), "windows");

    char *okcmd = bash_ok_command(kind);
    char *res = agentc_tool_bash(okcmd, 0, NULL, &err);
    agentc_free(okcmd);
    if (shell_is(kind, "sh"))
        check("bash_ok", !err && res && contains(res, "one\ntwo\n") &&
                            contains(res, "exit code: 0"));
    else
        check("bash_ok", !err && res && contains(res, "one") && contains(res, "two") &&
                            contains(res, "exit code: 0"));
    agentc_free(res);

    /* exit 3 means the same thing in sh, cmd and PowerShell. */
    res = agentc_tool_bash("exit 3", 0, NULL, &err);
    check("bash_exit", err && res && contains(res, "exit code: 3"));
    agentc_free(res);

    volatile bool cancel = true;
    const char *cancelcmd =
        shell_is(kind, "sh")       ? "sleep 5"
        : shell_is(kind, "cmd")    ? "for /l %i in (1,1,2000000000) do @rem"
                                    : "Start-Sleep -Seconds 5";
    res = agentc_tool_bash(cancelcmd, 0, &cancel, &err);
    check("bash_cancel", err && res && contains(res, "[aborted]"));
    agentc_free(res);

    /* a background child inherits the pipe write end; the tool must stop when
     * the direct child exits instead of waiting for the grandchild */
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    const char *bgcmd =
        shell_is(kind, "sh") ? "sleep 30 & echo started"
        : shell_is(kind, "cmd")
            ? "start /b ping -n 31 127.0.0.1 >NUL & echo started"
            : "cmd /c 'start /b ping -n 31 127.0.0.1 >NUL & echo started'";
    res = agentc_tool_bash(bgcmd, 5000, NULL, &err);
    i64 elapsed_ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    bool bg_ok = !err && res && contains(res, "started") && elapsed_ms < 5000;
    if (win) bg_ok = bg_ok && bash_alt_shell_ok(kind);
    check("bash_background", bg_ok);
    agentc_free(res);

    AgcBuf cmd = { 0 };
    if (shell_is(kind, "sh"))
        agentc_buf_printf(&cmd, "cat %s/big.txt", TDIR);
    else if (shell_is(kind, "cmd"))
        agentc_buf_printf(&cmd, "type \"%s\\big.txt\"", win_tdir());
    else
        agentc_buf_printf(&cmd, "Get-Content '%s\\big.txt'", win_tdir());
    res = agentc_tool_bash((const char *)cmd.p, 0, NULL, &err);
    agentc_buf_free(&cmd);
    check("bash_truncated",
          !err && res && contains(res, "[output truncated:") && contains(res, "full output: "));
    const char *p = agentc_str_str(res, "full output: ");
    bool spill_ok = false;
    char spill_path[4096] = "";
    if (p) {
        p += 13;
        size_t k = 0;
        while (p[k] && p[k] != ']' && k < sizeof spill_path - 1) {
            spill_path[k] = p[k];
            k++;
        }
        spill_path[k] = 0;
        int fd = os_open(spill_path, OS_O_RDONLY, 0);
        spill_ok = fd >= 0;
        if (fd >= 0) os_close(fd);
    }
    check("bash_spill_exists", spill_ok);
    bool spill_mode = true;
    if (!agentc_streq(os_platform(), "windows")) {
        struct os_stat st;
        spill_mode = spill_path[0] && os_stat(spill_path, &st) == 0 &&
                     (st.st_mode & 0777) == 0600;
    }
    check("bash_spill_mode", spill_mode);
    agentc_free(res);

    /* Lifetime cases, appended so the legacy bash_* labels above stay
     * byte-identical. */
    test_bash_eof_hang(kind);
    test_bash_group_kill(win);
}

static void test_registry_args(void) {
    AgcTool tools[8];
    size_t n = agentc_tools_builtin(tools, 8);
    check("tools_count", n == 7);

    bool err = false;
    char *res = run_tool(&tools[0], "{\"path\":123}", &err);
    check("arg_read_path_type", err && contains(res, "missing required field"));
    agentc_free(res);

    res = run_tool(&tools[0], "[]", &err);
    check("arg_read_root", err && contains(res, "must be a JSON object"));
    agentc_free(res);

    res = run_tool(&tools[0], "{\"path\":\"" TDIR "/a.txt\",\"limit\":\"x\"}", &err);
    check("arg_read_limit_type", err && contains(res, "must be numbers"));
    agentc_free(res);

    res = run_tool(&tools[1], "{\"command\":\"true\",\"timeout\":\"x\"}", &err);
    check("arg_bash_timeout_type", err && contains(res, "timeout must be a number"));
    agentc_free(res);

    /* the v2 entry points, not the retired exec shim */
    res = run_tool(&tools[0], "{}", &err);
    check("tools.run.missing-arg", err && contains(res, "missing required field: path"));
    agentc_free(res);

    res = run_tool(&tools[0], "[]", &err);
    check("tools.run.bad-root", err && contains(res, "must be a JSON object"));
    agentc_free(res);

    const char *kind = selected_shell();
    res = run_tool(&tools[1], "{\"command\":\"echo ok\"}", &err);
    check("tools.bash.run",
          !err && res && contains(res, "ok") && contains(res, "exit code: 0"));
    agentc_free(res);

    const char *cancelcmd =
        shell_is(kind, "sh")    ? "sleep 5"
        : shell_is(kind, "cmd") ? "for /l %i in (1,1,2000000000) do @rem"
                                 : "Start-Sleep -Seconds 5";
    char args[160];
    agentc_snprintf(args, sizeof args, "{\"command\":\"%s\"}", cancelcmd);
    volatile bool cancel = false;
    res = run_tool_cancel(&tools[1], args, &cancel, &err);
    check("tools.bash.cancel-run", err && res && contains(res, "[aborted]"));
    agentc_free(res);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    (void)os_mkdir(TDIR, 0755);
    os_unlink(TDIR "/a.txt");
    os_unlink(TDIR "/big.txt");
    os_unlink(TDIR "/multi.txt");
    os_unlink(TDIR "/dup.txt");
    os_unlink(TDIR "/crlf.txt");

    test_write_read();
    test_write_mode();
    test_write_len();
    test_read_truncation();
    test_read_limits();
    test_edit();
    test_bash();
    test_registry_args();

    return fails;
}
