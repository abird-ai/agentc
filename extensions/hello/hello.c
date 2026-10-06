/* hello.c — minimal C extension: one echo tool, one hook and one status
 * segment.
 *
 * Build (see extensions/README.md):
 *   clang -std=c23 -ffreestanding ... -I include \
 *        -Dagentc_ext_init=agentc_ext_hello_init -c extensions/hello/hello.c \
 *       -o build/extensions/hello.o
 *
 * -DHELLO_VETO=1 makes the tool_call handler veto payloads with
 * {"input":{"veto":true}}; tests/ext.sh builds this variant to exercise the
 * veto return path.
 */
#include "agentc_ext.h"

static const AgcExtHost *g_host;

static const char hello_name[] = "hello";
static const char hello_version[] = "0.1.0";

static const char echo_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"text\":{\"type\":\"string\",\"description\":\"Text to echo back\"}},"
    "\"required\":[\"text\"]}";

/* Tools are synchronous: append plain result text with host->out_write and
 * return 0 | -errno. There is no JSON content envelope any more. */
static int hello_run(const AgcExtHost *host, const AgcExtTool *self,
                     const AgcExtToolCall *call, void *out, bool *is_error) {
    (void)self;
    const char *text = host->json_get_str(call->args_json, "text", NULL);
    if (!text) {
        static const char missing[] =
            "error: hello_echo: missing required field: text";
        host->out_write(out, missing, sizeof missing - 1);
        *is_error = true;
        return 0;
    }
    size_t n = 0;
    while (text[n]) n++;
    host->out_write(out, text, n);
    return 0;
}

/* tool_call is an OVERRIDE hook point: returning 1 vetoes the call. We never
 * set *result_json; the return value carries the decision. */
static int hello_on_tool_call(void *userdata, const char *point,
                              const char *payload_json, char **result_json) {
    (void)userdata;
    (void)point;
    (void)result_json;
#ifdef HELLO_VETO
    if (g_host->json_get_bool(payload_json, "input.veto", 0)) {
        g_host->log(2, "hello: vetoing tool_call");
        return 1;   /* veto */
    }
#else
    (void)payload_json;
#endif
    return 0;
}

/* Contributes one status-line segment: plain text, no escapes, the host owns
 * position, separators and truncation. userdata is the pointer handed to
 * add_status (NULL here). The provider may use `arena` for dynamic text. */
static size_t hello_status(void *userdata, AgcExtStatusSegment *out, size_t max,
                           char *arena, size_t arena_cap) {
    (void)userdata;
    (void)arena;
    (void)arena_cap;
    if (max < 1) return 0;
    out[0].struct_size = sizeof out[0];
    out[0].slot = AGENTC_PSEG_SLOT_LEFT;
    out[0].priority = 100;
    out[0].style = AGENTC_PSEG_STYLE_DIM;
    out[0].text = "hello";
    return 1;
}

static int hello_init(const AgcExtHost *host) {
    g_host = host;
    static const AgcExtTool tool = {
        .struct_size = sizeof(AgcExtTool),
        .flags = AGENTC_TOOL_READONLY,
        .name = "hello_echo",
        .label = "Hello echo",
        .description = "Echo the `text` argument back to the model.",
        .parameters_json = echo_params,
        .prompt_snippet = "hello_echo(text) - echo text back",
        .prompt_guidelines = NULL,
        .ud = NULL,
        .timeout_ms = 0,
        .run = hello_run,
    };
    host->add_tool(&tool);
    host->on(AGENTC_HEV_TOOL_CALL, AGENTC_HOOK_OVERRIDE, 0,
             hello_on_tool_call, NULL);
    host->add_status(hello_status, NULL);
    return 0;
}

int agentc_ext_init(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = hello_name;
    out->version = hello_version;
    out->order = 0;
    out->init = hello_init;
    out->shutdown = NULL;
    out->required_host_size = 0;   /* no host-service requirement */
    return 0;
}
