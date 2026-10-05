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
static int hello_run(const AgcExtHost *host, const AgcExtTool *self, const AgcExtToolCall *call,
                     void *out, bool *is_error) {
    (void)self;
    const char *text = host->json_get_str(call->args_json, "text", NULL);
    if (!text) {
        static const char missing[] = "error: hello_echo: missing required field: text";
        host->out_write(out, missing, sizeof missing - 1);
        *is_error = true;
        return 0;
    }
    size_t n = 0;
    while (text[n])
        n++;
    host->out_write(out, text, n);
    return 0;
}

/* tool_call is an OVERRIDE hook point: returning 1 vetoes the call. We never
 * set *result_json; the return value carries the decision. */
static int hello_on_tool_call(void *userdata, const char *point, const char *payload_json,
                              char **result_json) {
    (void)userdata;
    (void)point;
    (void)result_json;
#ifdef HELLO_VETO
    if (g_host->json_get_bool(payload_json, "input.veto", 0)) {
        g_host->log(2, "hello: vetoing tool_call");
        return 1; /* veto */
    }
#else
    (void)payload_json;
#endif
    return 0;
}

/* Contributes one status line: plain text, no escapes, the host owns position,
 * separators and truncation. userdata is the pointer handed to add_status_line
 * (NULL here). The render callback may write several lines. */
static int hello_status(const AgcExtHost *host, void *userdata, const AgcExtStatusCtx *ctx,
                        void *out) {
    (void)userdata;
    (void)ctx;
    host->out_write(out, "hello", 5);
    return 0;
}

/* A prompt contribution: append a hint before the Environment anchor. */
static int hello_prompt(const AgcExtHost *host, void *userdata, void *out) {
    (void)userdata;
    host->out_write(out, "\nhello: greeting tool available.\n", 32);
    return 0;
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
    host->on(AGENTC_HEV_TOOL_CALL, AGENTC_HOOK_OVERRIDE, 0, hello_on_tool_call, NULL);
    static const AgcExtStatusLine status = {
        .struct_size = sizeof(AgcExtStatusLine),
        .region = AGENTC_REGION_STATUS_LEFT,
        .lifetime = AGENTC_STATUS_PERSISTENT,
        .priority = 100,
        .style = AGENTC_PSEG_STYLE_DIM,
        .render = hello_status,
    };
    host->add_status_line(&status);
    static const AgcExtPrompt prompt = {
        .struct_size = sizeof(AgcExtPrompt),
        .key = "hello-hint",
        .priority = 50,
        .render = hello_prompt,
        .anchor = AGENTC_ANCHOR_ENVIRONMENT,
        .position = AGENTC_PROMPT_BEFORE,
        .mode = AGENTC_PROMPT_APPEND,
    };
    host->add_prompt(&prompt);
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
    out->required_host_size = 0; /* no host-service requirement */
    return 0;
}
