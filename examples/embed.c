/*
 * examples/embed.c — minimal libagentc host.
 *
 * Proves that build/libagentc.a links into a normal (default-flag PIE) process
 * with the host's libc, that the public SDK header is self-contained, and that
 * one init/shutdown cycle leaves process memory stable across a second cycle.
 *
 * Build (in-tree):  make libcheck
 * The output is compared against tests/data/embed.expected.
 */
#include "agentc_sdk.h"

#include <stdio.h>
#include <string.h>

static void on_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)ev;
    (void)data; /* a host would route these to its UI */
}

static int cycle(const char *tag, size_t *live_out) {
    AgcSdkConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.log_level = 4; /* quiet: the golden is the API trace */
    int rc = agentc_sdk_init(&cfg);
    printf("%s init: %s\n", tag, rc == 0 ? "ok" : agentc_sdk_last_error());
    if (rc != 0)
        return 1;

    printf("%s ready: %d\n", tag, agentc_sdk_ready());
    printf("%s tools: %zu\n", tag, agentc_sdk_tools(NULL, 0));
    printf("%s providers: %zu\n", tag, agentc_sdk_providers(NULL, 0));
    printf("%s provider(openai): %d\n", tag, agentc_sdk_provider("openai") != NULL);
    printf("%s needs_key(openai): %d\n", tag, (int)agentc_sdk_needs_key("openai"));
    printf("%s needs_key(ollama): %d\n", tag, (int)agentc_sdk_needs_key("ollama"));

    AgcSdkAgentOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = (uint32_t)sizeof opts;
    opts.thinking = -1;
    opts.provider = "ollama"; /* local provider: no credential needed */
    opts.model = "llama3";
    opts.events = on_event;
    AgcSdkAgent *a = agentc_sdk_agent_new(&opts);
    printf("%s agent: %s\n", tag, a ? "ok" : agentc_sdk_last_error());
    if (!a) {
        agentc_sdk_shutdown();
        return 1;
    }
    printf("%s agent tools>0: %d\n", tag,
           agentc_sdk_agent(a) != NULL && agentc_sdk_tools(NULL, 0) > 0);
    agentc_sdk_pump(a, 0);
    printf("%s transcript: %zu\n", tag, agentc_sdk_agent_transcript(a)->n);
    agentc_sdk_agent_free(a);
    agentc_sdk_shutdown();
    printf("%s shutdown: ok\n", tag);
    if (live_out)
        *live_out = agentc_mem_live();
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    size_t live1 = 0, live2 = 0;
    if (cycle("one", &live1) != 0)
        return 1;
    if (cycle("two", &live2) != 0)
        return 1;
    printf("mem stable: %d\n", live1 == live2);
    return live1 == live2 ? 0 : 1;
}
