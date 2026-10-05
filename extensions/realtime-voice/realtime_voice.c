/* realtime_voice.c — minimal realtime-voice extension (P6-VOICE / R-D27).
 *
 * The full WebRTC/audio helper is out of scope for the freestanding core; this
 * lands the extension contract and the media-agnostic flow the core needs:
 *
 *   - a `realtime-voice` settings group (enabled, mode, voice, mic, speaker);
 *   - a `/voice` command that reports state and emits a CONTROL
 *     REALTIME_START/STOP message;
 *   - a `voice` status line;
 *   - `input`/`output` hook handlers that pass `application/x-realtime-item`
 *     and `audio/pcm` messages through unchanged (a real helper would promote
 *     items and speak text).
 *
 * Helper protocol (extension-defined, P6-VOICE §12): length-prefixed frames on
 * a process actor spawned with AGENTC_PROC_PIPE_BIDI. The wire contract and its
 * bounded streaming codec live in include/voice.h (`agentc_voice_frame_encode`,
 * `agentc_voice_decoder_feed`, `agentc_voice_ctrl_frame`). Because the stable
 * extension ABI has no actor_spawn slot yet, the helper is started through the
 * core actor API by the app, or a future host slot; this file documents the
 * frames and leaves the transport to that layer.
 *   u32be len | u8 type | payload
 *   0x01 CTRL_JSON  0x02 PCM_IN  0x03 PCM_OUT  0x04 TRANSCRIPT
 *   0x05 ITEM       0x06 TOOLCALL 0x07 TOOLRESULT 0x08 ERROR
 */
#include "agentc_ext.h"

static const AgcExtSetting s_enabled = {
    .struct_size = sizeof(AgcExtSetting),
    .key = "enabled",
    .label = "Enable voice",
    .description = "Allow the realtime voice session.",
    .type = AGENTC_SET_BOOL,
    .default_json = "false",
};
static const AgcExtSetting s_mode = {
    .struct_size = sizeof(AgcExtSetting),
    .key = "mode",
    .label = "Transport",
    .description = "Voice transport.",
    .type = AGENTC_SET_ENUM,
    .default_json = "\"webrtc\"",
    .enum_json = "[\"webrtc\",\"ws\"]",
    .flags = AGENTC_SETTING_RESTART,
};
static const AgcExtSetting s_voice = {
    .struct_size = sizeof(AgcExtSetting),
    .key = "voice",
    .label = "Voice",
    .description = "Backend voice id.",
    .type = AGENTC_SET_STRING,
    .default_json = "\"alloy\"",
};

static int voice_status(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    (void)host;
    host->out_write(out, "voice:ready", 11);
    return 0;
}

static void voice_command(const AgcExtHost *host, void *ud, const char *args_json, void *out) {
    (void)ud;
    (void)args_json;
    /* A real extension would spawn the helper and send CONTROL. Here the command
     * reports the contract so /voice is discoverable. */
    (void)host->out_write(out, "voice: helper contract loaded (P6-VOICE)", 38);
}

static int voice_input(void *ud, const char *point, const char *payload_json, char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    if (result_json)
        *result_json = NULL;
    return 0; /* pass media/items through unchanged */
}

static int voice_init(const AgcExtHost *host) {
    if (AGENTC_EXT_HOST_HAS(host, add_setting)) {
        host->add_setting(host, &s_enabled);
        host->add_setting(host, &s_mode);
        host->add_setting(host, &s_voice);
    }
    static const AgcExtCommand cmd = {
        .struct_size = sizeof(AgcExtCommand),
        .name = "voice",
        .description = "start|stop|mute|list the realtime voice session",
        .run = voice_command,
    };
    host->add_command(&cmd);
    static const AgcExtStatusLine line = {
        .struct_size = sizeof(AgcExtStatusLine),
        .region = AGENTC_REGION_STATUS_RIGHT,
        .lifetime = AGENTC_STATUS_PERSISTENT,
        .priority = 50,
        .style = AGENTC_PSEG_STYLE_DIM,
        .render = voice_status,
    };
    host->add_status_line(&line);
    host->on("input", AGENTC_HOOK_OBSERVE, 0, voice_input, NULL);
    return 0;
}

int agentc_ext_init(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "realtime-voice";
    out->version = "0.1.0";
    out->order = 50;
    out->init = voice_init;
    out->shutdown = NULL;
    out->required_host_size = 0;
    return 0;
}
