/* ls.c — the ls tool: one directory, sorted, directories suffixed with "/".
 *
 * Reads the directory through agentc_dir_scan (dirent types, no symlink following)
 * and renders one name per line. `limit` truncates the listing with a notice.
 */
#include "agent.h"
#include "plat.h"
#include "core/tools/args.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

int agentc_dir_scan(const char *dir, int (*cb)(void *ud, const char *name, const char *full,
                                           bool is_dir),
                void *ud);
bool agentc_path_is_dir(const char *path);

#define LS_DEFAULT_LIMIT 200

typedef struct {
    AgcVec names;   /* owned char* (name + optional '/') */
} LsCtx;

static int ls_collect(void *ud, const char *name, const char *full, bool is_dir) {
    (void)full;
    LsCtx *c = ud;
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, name);
    if (is_dir) agentc_buf_byte(&b, '/');
    agentc_buf_byte(&b, 0);
    b.len--;
    *(char **)agentc_vec_push(&c->names, sizeof(char *)) = (char *)b.p;
    return 0;
}

static void sort_names(char **a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        char *key = a[i];
        size_t j = i;
        while (j > 0) {
            const char *x = a[j - 1], *y = key;
            size_t k = 0;
            while (x[k] && x[k] == y[k]) k++;
            int c = (int)(unsigned char)x[k] - (int)(unsigned char)y[k];
            if (c <= 0) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

char *agentc_tool_ls(const char *path, i64 limit, bool *is_error) {
    if (is_error) *is_error = false;
    const char *dir = (path && path[0]) ? path : ".";
    if (!agentc_path_is_dir(dir))
        return agentc_tool_read_err(is_error, "error: ls: not a directory: %s", dir);

    LsCtx c;
    agentc_memset(&c, 0, sizeof c);
    int rc = agentc_dir_scan(dir, ls_collect, &c);
    if (rc < 0 && c.names.len == 0) {
        for (size_t i = 0; i < c.names.len; i++) agentc_free(((char **)c.names.p)[i]);
        agentc_vec_free(&c.names);
        return agentc_tool_read_err(is_error, "error: ls: cannot read %s (errno %d)", dir, rc);
    }
    sort_names((char **)c.names.p, c.names.len);

    size_t lim = limit > 0 ? (size_t)limit : LS_DEFAULT_LIMIT;
    if (lim > 100000) lim = 100000;
    size_t show = c.names.len < lim ? c.names.len : lim;

    AgcBuf out = { 0 };
    for (size_t i = 0; i < show; i++) {
        agentc_buf_cstr(&out, ((char **)c.names.p)[i]);
        agentc_buf_byte(&out, '\n');
    }
    if (show < c.names.len)
        agentc_buf_printf(&out, "[listing truncated: showing %llu of %llu entries]\n",
                      (unsigned long long)show, (unsigned long long)c.names.len);
    for (size_t i = 0; i < c.names.len; i++) agentc_free(((char **)c.names.p)[i]);
    agentc_vec_free(&c.names);
    if (!out.p) return agentc_strdup_len("", 0);
    return (char *)out.p;
}

int agentc_tool_ls_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                       bool *is_error) {
    (void)self;
    if (is_error) *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return -12; /* ENOMEM */
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: ls: arguments must be a JSON object");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    AgcJson *lv = agentc_json_get(a.root, "limit");
    if (lv && agentc_json_type(lv) != AGENTC_JSON_NUM) {
        agentc_buf_cstr(out, "error: ls: limit must be a number");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    i64 limit = agentc_tool_args_int(&a, "limit", 0);
    bool err = false;
    char *res = agentc_tool_ls(path, limit, &err);
    if (res != NULL) {
        agentc_buf_cstr(out, res);
        agentc_free(res);
    }
    if (is_error) *is_error = err;
    agentc_tool_args_free(&a);
    return 0;
}
