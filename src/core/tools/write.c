/* write.c — the write tool: mkdir -p parents, atomic temp+rename replace. */
#include "agent.h"
#include "plat.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

static int mkdir_parents(const char *path) {
    char buf[4096];
    size_t n = agentc_strlen(path);
    if (n >= sizeof buf) return -36; /* ENAMETOOLONG */
    agentc_memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        int r = os_mkdir(buf, 0755);
        if (r < 0 && r != -17 /* EEXIST */) return r;
        buf[i] = '/';
    }
    return 0;
}

static int write_all_fd(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == -4) continue; /* EINTR */
            return w;
        }
        if (w == 0) return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

char *agentc_tool_write_len(const char *path, const char *content, size_t len,
                       bool *is_error) {
    if (is_error) *is_error = false;
    if (!path || !path[0]) return agentc_tool_read_err(is_error, "error: write: path is required");
    if (!content) len = 0;   /* a NULL pointer carries no bytes */

    int mr = mkdir_parents(path);
    if (mr < 0)
        return agentc_tool_read_err(is_error, "error: cannot create parent of %s (errno %d)", path,
                                mr);

    u64 rnd = 0;
    (void)os_random(&rnd, sizeof rnd);
    i64 now = os_now_ns(OS_CLOCK_MONOTONIC);
    char tmp[4200];
    int n = agentc_snprintf(tmp, sizeof tmp, "%s.agentc-%llx%llx.tmp", path,
                        (unsigned long long)now, (unsigned long long)rnd);
    if (n <= 0 || (size_t)n >= sizeof tmp)
        return agentc_tool_read_err(is_error, "error: path too long: %s", path);

    int mode = 0644;
    /* Preserve the target's permission bits across the atomic replace: editing a
     * 0600 secret must not widen it to 0644. edit.c goes through this function
     * too. A fresh file keeps the historical 0644. */
    struct os_stat st;
    if (os_stat(path, &st) == 0) mode = (int)(st.st_mode & 0777);
    int fd = os_open(tmp, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, mode);
    if (fd < 0)
        return agentc_tool_read_err(is_error, "error: cannot create %s (errno %d)", tmp, fd);
    int w = write_all_fd(fd, content, len);
    int cr = os_close(fd);
    if (w < 0 || cr < 0) {
        os_unlink(tmp);
        return agentc_tool_read_err(is_error, "error: write %s failed (errno %d)", path,
                                w < 0 ? w : cr);
    }
    int rr = os_rename(tmp, path);
    if (rr < 0) {
        os_unlink(tmp);
        return agentc_tool_read_err(is_error, "error: rename %s failed (errno %d)", path, rr);
    }

    AgcBuf out = { 0 };
    agentc_buf_printf(&out, "wrote %llu bytes to %s\n", (unsigned long long)len, path);
    return (char *)out.p;
}

char *agentc_tool_write(const char *path, const char *content, bool *is_error) {
    return agentc_tool_write_len(path, content, content ? agentc_strlen(content) : 0, is_error);
}
