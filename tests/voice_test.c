/* voice_test.c — media-agnostic message flow and BIDI process actors (P6-E).
 *
 * Covers the P6-VOICE core additions: AgcMessage.content_type JSON round-trip
 * (default text/plain, audio/realtime values preserved), the
 * AGENTC_MSG_REALTIME_START/STOP control kinds, the AGENTC_PROC_PIPE_BIDI
 * writable-stdin mode (and that a plain PIPE rejects writes), and the
 * mic/speaker trust flags being denied by default. On Windows the BIDI echo
 * check is a documented skip (tests/data/voice_test.windows.expected).
 */
#include "agentc.h"
#include "actor.h"
#include "message.h"
#include "trust.h"
#include "plat.h"
#include "voice.h"

static int fails;
static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* The Windows `more` stub does not echo predictably; document the skip rather
 * than a hard-coded 1 (tests/data/voice_test.windows.expected). */
static void skip(const char *label) {
    agentc_outf("%s=skip\n", label);
}

static void test_content_type(void) {
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_INPUT;
    m.kind = AGENTC_MSG_INPUT;
    m.to = 0;
    m.body = "hello";
    m.body_len = 5;
    m.content_type = "audio/pcm;rate=24000;ch=1";
    AgcBuf b = {0};
    check("ct_to_json", agentc_message_to_json(&m, &b) == 0);
    check("ct_contains", b.p && agentc_str_str((char *)b.p, "audio/pcm") != NULL);

    AgcMessage back;
    check("ct_from_json", agentc_message_from_json((char *)b.p, b.len, &back) == 0);
    check("ct_roundtrip",
          back.content_type && agentc_streq(back.content_type, "audio/pcm;rate=24000;ch=1"));
    agentc_message_free(&back);
    agentc_buf_free(&b);

    /* Absent content_type defaults to text/plain. */
    AgcMessage d;
    const char *dj = "{\"class\":\"input\",\"kind\":\"input\",\"body\":\"x\"}";
    check("ct_default_from_json", agentc_message_from_json(dj, agentc_strlen(dj), &d) == 0);
    check("ct_default", d.content_type && agentc_streq(d.content_type, "text/plain"));
    agentc_message_free(&d);
}

static void test_realtime_kinds(void) {
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_CONTROL;
    m.kind = AGENTC_MSG_REALTIME_START;
    m.to = 0;
    m.body = "{\"voice\":\"alloy\"}";
    m.body_len = agentc_strlen(m.body);
    AgcBuf b = {0};
    check("realtime_to_json", agentc_message_to_json(&m, &b) == 0);
    check("realtime_name", b.p && agentc_str_str((char *)b.p, "realtime_start") != NULL);
    AgcMessage back;
    check("realtime_from_json", agentc_message_from_json((char *)b.p, b.len, &back) == 0);
    check("realtime_roundtrip", back.kind == AGENTC_MSG_REALTIME_START);
    agentc_message_free(&back);
    agentc_buf_free(&b);
}

static void test_bidi_pipe(bool win) {
    agentc_actor_init();
    AgcProcSpawn spec;
    agentc_memset(&spec, 0, sizeof spec);
    spec.command = win ? "more" : "cat";
    spec.mode = AGENTC_PROC_PIPE_BIDI;
    spec.label = "voice-mock";
    char err[128] = "";
    AgcActorId id = agentc_actor_spawn(&(AgcActorSpec){.proc = &spec}, err, sizeof err);
    check("bidi_spawn", id != 0);
    if (!id) {
        agentc_actor_release_all();
        return;
    }
    check("bidi_stdin_open", agentc_process_write(id, "ping\n", 5, false) == 0);

    AgcBuf out = {0};
    for (int i = 0; i < 500 && out.len < 5; i++) {
        agentc_actor_tick();
        agentc_process_read(id, &out, 4096);
        os_poll(NULL, 0, 5);
    }
    if (win)
        skip("bidi_echo");
    else
        check("bidi_echo", out.p && agentc_str_str((char *)out.p, "ping") != NULL);
    agentc_buf_free(&out);
    agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 2);
    agentc_actor_tick();
    agentc_actor_release_all();

    /* A plain PIPE has no stdin: a write is refused. */
    AgcProcSpawn plain;
    agentc_memset(&plain, 0, sizeof plain);
    plain.command = win ? "more" : "cat";
    plain.mode = AGENTC_PROC_PIPE;
    char err2[128] = "";
    AgcActorId p = agentc_actor_spawn(&(AgcActorSpec){.proc = &plain}, err2, sizeof err2);
    check("pipe_spawn", p != 0);
    if (p) {
        check("pipe_write_denied", agentc_process_write(p, "x", 1, false) != 0);
        agentc_actor_cancel(p, AGENTC_FREEZE_ACTOR, 2);
        agentc_actor_tick();
    }
    agentc_actor_release_all();
}

/* ---- framed helper protocol (P6-VOICE §12) ---- */

typedef struct {
    size_t n;
    u8 type[8];
    char body[64];
    size_t len[8];
} FrameLog;

static int frame_cb(void *ud, const AgcVoiceFrame *f) {
    FrameLog *l = ud;
    if (l->n < 8) {
        l->type[l->n] = f->type;
        size_t c = f->len < sizeof l->body - 1 ? f->len : sizeof l->body - 1;
        if (c)
            agentc_memcpy(l->body + (l->n * 8), f->payload, c);
        l->body[l->n * 8 + c] = 0;
        l->len[l->n] = f->len;
    }
    l->n++;
    return 0;
}

static void test_frame_codec(void) {
    AgcBuf enc = {0};
    check("vf_ctrl_encode", agentc_voice_ctrl_frame("hi", &enc) == 0);
    /* u32be len=2 | type=0x01 | 'h' 'i' */
    check("vf_wire_shape", enc.len == 7 && enc.p[0] == 0 && enc.p[1] == 0 && enc.p[2] == 0 &&
                               enc.p[3] == 2 && enc.p[4] == AGENTC_VOICE_CTRL_JSON &&
                               enc.p[5] == 'h' && enc.p[6] == 'i');
    check("vf_type_name",
          agentc_streq(agentc_voice_frame_type_name(AGENTC_VOICE_PCM_IN), "pcm_in") &&
              agentc_streq(agentc_voice_frame_type_name(0xff), "unknown"));
    check("vf_encode_bad", agentc_voice_frame_encode(0x02, NULL, 1, &enc) == -22);
    check("vf_encode_oversize",
          agentc_voice_frame_encode(0x02, "x", AGENTC_VOICE_FRAME_MAX_BYTES + 1, &enc) == -90);
    agentc_buf_free(&enc);

    /* A stream of three frames (one empty) fed byte-by-byte must decode in
     * order regardless of the read split. */
    AgcBuf wire = {0};
    check("vf_stream_encode",
          agentc_voice_frame_encode(AGENTC_VOICE_PCM_IN, "ab", 2, &wire) == 0 &&
              agentc_voice_frame_encode(AGENTC_VOICE_TRANSCRIPT, "", 0, &wire) == 0 &&
              agentc_voice_frame_encode(0x7f, "zzz", 3, &wire) == 0);
    AgcVoiceDecoder *dec = agentc_alloc(sizeof *dec);
    agentc_voice_decoder_init(dec);
    FrameLog log;
    agentc_memset(&log, 0, sizeof log);
    int rc = 0;
    for (size_t i = 0; i < wire.len; i++) {
        rc = agentc_voice_decoder_feed(dec, wire.p + i, 1, frame_cb, &log);
        if (rc != 0)
            break;
    }
    check("vf_stream_rc", rc == 0);
    check("vf_stream_count", log.n == 3);
    check("vf_stream_types", log.type[0] == AGENTC_VOICE_PCM_IN &&
                                 log.type[1] == AGENTC_VOICE_TRANSCRIPT && log.type[2] == 0x7f);
    check("vf_stream_bodies", agentc_streq(log.body + 0, "ab") && log.len[1] == 0 &&
                                  agentc_streq(log.body + 16, "zzz"));
    agentc_free(dec);
    agentc_buf_free(&wire);

    /* A declared payload over the cap is rejected before it is buffered and
     * latches the decoder dead (fail-closed). */
    u8 bad[5] = {0xff, 0xff, 0xff, 0xff, AGENTC_VOICE_PCM_IN};
    AgcVoiceDecoder *d2 = agentc_alloc(sizeof *d2);
    agentc_voice_decoder_init(d2);
    FrameLog l2;
    agentc_memset(&l2, 0, sizeof l2);
    check("vf_oversize_reject",
          agentc_voice_decoder_feed(d2, bad, sizeof bad, frame_cb, &l2) == -90 && l2.n == 0);
    check("vf_dead_latches", agentc_voice_decoder_feed(d2, bad, 1, frame_cb, &l2) == -22);
    agentc_free(d2);

    /* A partial frame reports its pending bytes and a callback can stop the
     * feed with its own code. */
    AgcVoiceDecoder *d3 = agentc_alloc(sizeof *d3);
    agentc_voice_decoder_init(d3);
    u8 one[5] = {0, 0, 0, 1, AGENTC_VOICE_CTRL_JSON};
    check("vf_partial", agentc_voice_decoder_feed(d3, one, sizeof one, frame_cb, &l2) == 0 &&
                            agentc_voice_decoder_pending(d3) == 5);
    agentc_free(d3);
}

static void test_trust_voice_flags(void) {
    AgcTrustEnv env;
    agentc_trust_default(&env, "/tmp");
    check("mic_denied_by_default", (env.flags & AGENTC_TRUST_ALLOW_MIC) == 0);
    check("speaker_denied_by_default", (env.flags & AGENTC_TRUST_ALLOW_SPEAKER) == 0);
    check("pty_still_allowed", (env.flags & AGENTC_TRUST_ALLOW_PTY) != 0);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_content_type();
    test_realtime_kinds();
    test_bidi_pipe(agentc_streq(os_platform(), "windows"));
    test_frame_codec();
    test_trust_voice_flags();
    agentc_outf("voice tests: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
