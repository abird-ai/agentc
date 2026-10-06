/* safety_test.c — terminal-safe output: control-byte classification, lenient
 * UTF-8 sanitizing and the real writer paths (agentc_out, agentc_logf).
 *
 * The golden carries the sanitized bytes: the labels printed with the writer
 * proof are fed raw ESC/DEL input, so a regression that lets a control byte
 * through changes those lines instead of hiding behind a hex dump.
 */
#include "agentc.h"
#include "base/out.h"
#include "wire.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* Space-separated lowercase hex on one line; the sanitizer's U+FFFD output
 * (ef bf bd) is shown as one token, matching the contract's notation. */
static void hexline(const char *label, const AgcBuf *b) {
    static const u8 repl[3] = { 0xef, 0xbf, 0xbd };
    agentc_outf("%s=", label);
    bool first = true;
    for (size_t i = 0; i < b->len;) {
        if (i + 3 <= b->len && agentc_memeq(b->p + i, repl, 3)) {
            agentc_outf("%sefbfbd", first ? "" : " ");
            i += 3;
        } else {
            agentc_outf("%s%02x", first ? "" : " ", b->p[i]);
            i++;
        }
        first = false;
    }
    agentc_out_nl();
}

static void safe_hex(const char *label, const char *s) {
    AgcBuf b = { 0 };
    agentc_out_safe_buf(&b, s, agentc_strlen(s));
    hexline(label, &b);
    agentc_buf_free(&b);
}

/* ASCII JSON escaping proof: every emitted byte must be < 0x7f and the escape
 * text must match exactly. */
static void json_ascii_check(const char *label, const char *in, size_t n,
                             const char *want) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_set_ascii(&w, true);
    agentc_jsonw_str(&w, in, n);
    bool ascii_only = true;
    for (size_t i = 0; i < b.len; i++)
        if (b.p[i] >= 0x7fu) ascii_only = false;
    check(label, ascii_only && b.len == agentc_strlen(want) &&
                     agentc_memeq(b.p, want, b.len));
    agentc_buf_free(&b);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    size_t base = agentc_mem_live();

    check("safety.out.esc", !agentc_out_cp_safe(0x1b));
    check("safety.out.del", !agentc_out_cp_safe(0x7f));
    check("safety.out.c1", !agentc_out_cp_safe(0x9b));
    check("safety.out.bel", !agentc_out_cp_safe(0x07));
    check("safety.out.nl", agentc_out_cp_safe('\n'));
    check("safety.out.tab", agentc_out_cp_safe('\t'));
    check("safety.out.cr", agentc_out_cp_safe('\r'));
    check("safety.out.utf8", agentc_out_cp_safe(0xe9));
    check("safety.out.filter_unsafe", agentc_out_cp_filter(0x1b) == 0xfffd);
    check("safety.out.filter_safe", agentc_out_cp_filter(0xe9) == 0xe9);

    safe_hex("safety.out.osc", "\x1b]52;c;AAAA\x07");
    safe_hex("safety.out.csi", "\x1b[31m");
    safe_hex("safety.out.del", "\x7f");
    safe_hex("safety.out.c1raw", "\x9b" "31m");
    safe_hex("safety.out.c1utf8", "\xc2\x9b");
    safe_hex("safety.out.keeps", "a\n\t\r b\xc3\xa9");

    /* ascii JSON: C1, BMP, non-BMP (surrogate pair), a control byte, a plain
     * ASCII run and a lone invalid lead byte (lenient -> its own value) */
    json_ascii_check("safety.json.c1", "\xc2\x9b", 2, "\"\\u009b\"");
    json_ascii_check("safety.json.e9", "\xc3\xa9", 2, "\"\\u00e9\"");
    json_ascii_check("safety.json.nonbmp", "\xf0\x9f\x98\x80", 4,
                     "\"\\ud83d\\ude00\"");
    json_ascii_check("safety.json.ctrl", "\x01", 1, "\"\\u0001\"");
    json_ascii_check("safety.json.ascii", "ab", 2, "\"ab\"");
    json_ascii_check("safety.json.lone-lead", "\xe9", 1, "\"\\u00e9\"");

    /* feed the raw control bytes through the real writer: only the sanitized
     * bytes may appear in the golden */
    agentc_outs("safety.out.writer_osc=");
    agentc_out("\x1b]52;c;AAAA\x07", 12);
    agentc_out_nl();
    agentc_outs("safety.out.writer_csi=");
    agentc_out("\x1b[31m", 5);
    agentc_out_nl();
    agentc_outs("safety.out.writer_del=");
    agentc_out("\x7f", 1);
    agentc_out_nl();
    agentc_outs("safety.out.log=");
    agentc_logf(3, "%s", "\x1b[31m");   /* stderr, merged by run.sh */

    agentc_outf("safety.out.mem_live=%d\n", (int)(agentc_mem_live() - base));
    return fails;
}
