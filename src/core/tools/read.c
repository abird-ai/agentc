/* read.c — the read tool: line/byte truncated file reads.
 *
 * Reads the whole file, then emits up to 2000 lines / 50 KB, line-aligned,
 * with a continuation notice pointing at the next offset.
 */
#include "agent.h"
#include "plat.h"
#include "base/limits.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...) {
    if (is_error) *is_error = true;
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf_used(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    return agentc_strdup_len(tmp, (size_t)n);
}

static size_t count_lines(const u8 *p, size_t n) {
    size_t lines = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '\n') lines++;
    if (n && p[n - 1] != '\n') lines++;
    return lines;
}

char *agentc_tool_read(const char *path, i64 offset, i64 limit, bool *is_error) {
    if (is_error) *is_error = false;
    if (!path || !path[0]) return agentc_tool_read_err(is_error, "error: read: path is required");

    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return agentc_tool_read_err(is_error, "error: cannot open %s (errno %d)", path, fd);

    struct os_stat st;
    if (os_fstat(fd, &st) == 0 && st.st_size > (i64)AGENTC_LIMIT_READ_BYTES) {
        os_close(fd);
        return agentc_tool_read_err(is_error,
                                "error: read %s: file is larger than 64 MiB; use grep or bash",
                                path);
    }
    AgcBuf f = { 0 };
    char tmp[65536];
    for (;;) {
        if (f.len > AGENTC_LIMIT_READ_BYTES) {
            os_close(fd);
            agentc_buf_free(&f);
            return agentc_tool_read_err(is_error, "error: read %s: file is larger than 64 MiB", path);
        }
        int n = os_read(fd, tmp, sizeof tmp);
        if (n < 0) {
            os_close(fd);
            agentc_buf_free(&f);
            return agentc_tool_read_err(is_error, "error: read %s failed (errno %d)", path, n);
        }
        if (n == 0) break;
        agentc_buf_push(&f, tmp, (size_t)n);
    }
    os_close(fd);

    size_t total_lines = count_lines(f.p, f.len);
    u64 off = offset > 0 ? (u64)offset : 0;
    u64 lim = limit > 0 ? (u64)limit : AGENTC_LIMIT_TOOL_LINES;
    if (lim > AGENTC_LIMIT_TOOL_LINES) lim = AGENTC_LIMIT_TOOL_LINES;

    AgcBuf out = { 0 };
    size_t pos = 0;
    u64 line = 0;
    while (pos < f.len && line < off) {
        while (pos < f.len && f.p[pos] != '\n') pos++;
        if (pos < f.len) pos++;
        line++;
    }
    u64 first = line;
    while (pos < f.len && line - first < lim && out.len < AGENTC_LIMIT_TOOL_BYTES) {
        size_t start = pos;
        while (pos < f.len && f.p[pos] != '\n') pos++;
        if (pos < f.len) pos++;
        size_t nline = pos - start;
        size_t room = AGENTC_LIMIT_TOOL_BYTES - out.len;
        if (nline > room) {
            /* never blow the byte budget on one giant line */
            agentc_buf_push(&out, f.p + start, room);
            agentc_buf_cstr(&out, "\n[line truncated at the byte limit]\n");
            line++;
            break;
        }
        agentc_buf_push(&out, f.p + start, nline);
        line++;
    }
    u64 end = line;
    bool truncated = end < total_lines;

    if (truncated) {
        if (out.len && out.p[out.len - 1] != '\n') agentc_buf_byte(&out, '\n');
        agentc_buf_printf(&out,
                      "[Showing lines %llu-%llu of %llu. Use offset=%llu to continue.]\n",
                      (unsigned long long)(first + 1), (unsigned long long)end,
                      (unsigned long long)total_lines, (unsigned long long)end);
    }

    agentc_buf_free(&f);
    if (!out.p) return agentc_strdup_len("", 0);
    return (char *)out.p;
}
