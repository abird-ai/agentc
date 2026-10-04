/* edit.c — the edit tool: unique oldText matches, CRLF/BOM preserving,
 * atomic write, right-to-left application, short hunk summary.
 */
#include "agent.h"
#include "plat.h"
#include "base/limits.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

typedef struct {
    char *old;
    size_t old_len;
    char *nw;
    size_t nw_len;
    i64 pos;
} Edit;

/* CRLF -> LF, so matching and output can assume plain newlines. */
static char *norm_alloc(const char *s, size_t n, size_t *out_len) {
    char *out = agentc_alloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') continue;
        out[o++] = s[i];
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

static i64 find_unique(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    i64 p = agentc_str_find(hay, hlen, needle, nlen);
    if (p < 0) return -1;
    if (agentc_str_find(hay + p + 1, hlen - (size_t)p - 1, needle, nlen) >= 0) return -2;
    return p;
}

static int read_file(const char *path, AgcBuf *out) {
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return fd;
    struct os_stat st;
    if (os_fstat(fd, &st) == 0 && st.st_size > (i64)AGENTC_LIMIT_EDIT_BYTES) {
        os_close(fd);
        return -27;                               /* EFBIG */
    }
    char tmp[65536];
    for (;;) {
        if (out->len > AGENTC_LIMIT_EDIT_BYTES) {
            os_close(fd);
            return -27;                           /* EFBIG: refuse, do not truncate */
        }
        int n = os_read(fd, tmp, sizeof tmp);
        if (n < 0) {
            os_close(fd);
            return n;
        }
        if (n == 0) break;
        agentc_buf_push(out, tmp, (size_t)n);
    }
    os_close(fd);
    return 0;
}

static size_t line_count(const char *s, size_t n) {
    if (!s || n == 0) return 0;
    size_t lines = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') lines++;
    if (s[n - 1] != '\n') lines++;
    return lines;
}

static void hunk_lines(AgcBuf *sum, const char *s, size_t n, char sign) {
    if (!s || n == 0) return;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '\n') {
            if (i == n && start == i) break;
            agentc_buf_byte(sum, (u8)sign);
            agentc_buf_push(sum, s + start, i - start);
            agentc_buf_byte(sum, '\n');
            start = i + 1;
        }
    }
}

char *agentc_tool_edit(const char *path, const char *edits_json, bool *is_error) {
    if (is_error) *is_error = false;
    if (!path || !path[0]) return agentc_tool_read_err(is_error, "error: edit: path is required");
    if (!edits_json) return agentc_tool_read_err(is_error, "error: edit: edits is required");

    AgcJsonArena *arena = agentc_json_arena_new(AGENTC_LIMIT_TOOL_ARENA);
    AgcJson *root = agentc_json_parse_in(arena, edits_json, agentc_strlen(edits_json));
    if (agentc_json_type(root) == AGENTC_JSON_OBJ) root = agentc_json_get(root, "edits");
    if (agentc_json_type(root) != AGENTC_JSON_ARR) {
        agentc_json_arena_free(arena);
        return agentc_tool_read_err(is_error, "error: edits must be a JSON array");
    }
    size_t ne = agentc_json_len(root);
    if (ne == 0) {
        agentc_json_arena_free(arena);
        return agentc_tool_read_err(is_error, "error: edits must not be empty");
    }

    Edit *ed = agentc_alloc(ne * sizeof(Edit));
    bool crlf = false;
    AgcBuf raw = { 0 }, norm = { 0 }, replaced = { 0 }, final = { 0 }, sum = { 0 };
    size_t *order = NULL;
    char *wres = NULL;
    size_t bom_len = 0;
    char *ret = NULL;

    for (size_t i = 0; i < ne; i++) {
        AgcJson *it = agentc_json_at(root, i);
        if (agentc_json_type(it) != AGENTC_JSON_OBJ) {
            ret = agentc_tool_read_err(is_error, "error: edit %llu must be an object",
                                   (unsigned long long)i);
            goto out;
        }
        size_t olen = 0, nlen = 0;
        const char *o = agentc_json_str(agentc_json_get(it, "oldText"), &olen);
        const char *nw = agentc_json_str(agentc_json_get(it, "newText"), &nlen);
        if (!o || olen == 0) {
            ret = agentc_tool_read_err(is_error,
                                   "error: edit %llu: oldText must be a non-empty string",
                                   (unsigned long long)i);
            goto out;
        }
        if (!nw) {
            ret = agentc_tool_read_err(is_error, "error: edit %llu: newText must be a string",
                                   (unsigned long long)i);
            goto out;
        }
        ed[i].old = norm_alloc(o, olen, &ed[i].old_len);
        ed[i].nw = norm_alloc(nw, nlen, &ed[i].nw_len);
    }

    int rr = read_file(path, &raw);
    if (rr < 0) {
        ret = agentc_tool_read_err(is_error, "error: cannot read %s (errno %d)", path, rr);
        goto out;
    }
    if (raw.len >= 3 && raw.p[0] == 0xEF && raw.p[1] == 0xBB && raw.p[2] == 0xBF) bom_len = 3;
    bool has_bom = bom_len > 0;
    for (size_t i = bom_len; i + 1 < raw.len; i++) {
        if (raw.p[i] == '\r' && raw.p[i + 1] == '\n') {
            crlf = true;
            break;
        }
    }
    for (size_t i = bom_len; i < raw.len;) {
        if (raw.p[i] == '\r' && i + 1 < raw.len && raw.p[i + 1] == '\n') {
            agentc_buf_byte(&norm, '\n');
            i += 2;
        } else {
            agentc_buf_byte(&norm, raw.p[i]);
            i++;
        }
    }

    for (size_t i = 0; i < ne; i++) {
        i64 p = find_unique((const char *)norm.p, norm.len, ed[i].old, ed[i].old_len);
        if (p == -1) {
            ret = agentc_tool_read_err(is_error, "error: edit %llu: oldText not found in %s",
                                   (unsigned long long)i, path);
            goto out;
        }
        if (p == -2) {
            ret = agentc_tool_read_err(is_error, "error: edit %llu: oldText is not unique in %s",
                                   (unsigned long long)i, path);
            goto out;
        }
        ed[i].pos = p;
    }
    for (size_t i = 0; i < ne; i++) {
        for (size_t j = i + 1; j < ne; j++) {
            bool overlap = ed[j].pos < ed[i].pos + (i64)ed[i].old_len &&
                           ed[i].pos < ed[j].pos + (i64)ed[j].old_len;
            if (overlap) {
                ret = agentc_tool_read_err(is_error,
                                       "error: edits %llu and %llu overlap in %s",
                                       (unsigned long long)i, (unsigned long long)j, path);
                goto out;
            }
        }
    }

    order = agentc_alloc(ne * sizeof(size_t));
    for (size_t i = 0; i < ne; i++) order[i] = i;
    for (size_t i = 1; i < ne; i++) {
        size_t v = order[i];
        size_t j = i;
        while (j > 0 && ed[order[j - 1]].pos > ed[v].pos) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }

    size_t cur = 0;
    for (size_t k = 0; k < ne; k++) {
        size_t idx = order[k];
        agentc_buf_push(&replaced, norm.p + cur, (size_t)ed[idx].pos - cur);
        agentc_buf_push(&replaced, ed[idx].nw, ed[idx].nw_len);
        cur = (size_t)ed[idx].pos + ed[idx].old_len;
    }
    if (cur < norm.len) agentc_buf_push(&replaced, norm.p + cur, norm.len - cur);

    if (crlf) {
        for (size_t i = 0; i < replaced.len; i++) {
            if (replaced.p[i] == '\n')
                agentc_buf_cstr(&final, "\r\n");
            else
                agentc_buf_byte(&final, replaced.p[i]);
        }
    } else {
        agentc_buf_push(&final, replaced.p, replaced.len);
    }

    bool werr = false;
    const char *outp = (const char *)final.p;
    if (!outp) outp = "";
    if (has_bom) {
        AgcBuf with_bom = { 0 };
        agentc_buf_push(&with_bom, "\xEF\xBB\xBF", 3);
        agentc_buf_push(&with_bom, outp, final.len);
        wres = agentc_tool_write(path, (const char *)with_bom.p, &werr);
        agentc_buf_free(&with_bom);
    } else {
        wres = agentc_tool_write(path, outp, &werr);
    }
    if (werr) {
        ret = wres;
        wres = NULL;
        goto out;
    }

    agentc_buf_printf(&sum, "edited %s: %llu edit(s)\n", path, (unsigned long long)ne);
    for (size_t k = 0; k < ne; k++) {
        size_t idx = order[k];
        size_t line = 1;
        for (size_t i = 0; i < (size_t)ed[idx].pos; i++)
            if (norm.p[i] == '\n') line++;
        agentc_buf_printf(&sum, "@@ -%llu,%llu +%llu,%llu @@\n", (unsigned long long)line,
                      (unsigned long long)line_count(ed[idx].old, ed[idx].old_len),
                      (unsigned long long)line,
                      (unsigned long long)line_count(ed[idx].nw, ed[idx].nw_len));
        hunk_lines(&sum, ed[idx].old, ed[idx].old_len, '-');
        hunk_lines(&sum, ed[idx].nw, ed[idx].nw_len, '+');
    }
    ret = (char *)sum.p;
    sum.p = NULL;

out:
    for (size_t i = 0; i < ne; i++) {
        agentc_free(ed[i].old);
        agentc_free(ed[i].nw);
    }
    agentc_free(ed);
    agentc_free(order);
    agentc_free(wres);
    agentc_json_arena_free(arena);
    agentc_buf_free(&raw);
    agentc_buf_free(&norm);
    agentc_buf_free(&replaced);
    agentc_buf_free(&final);
    agentc_buf_free(&sum);
    return ret;
}
