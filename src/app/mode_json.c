/* mode_json.c — `--mode json`: one prompt, JSONL events on the injected writer.
 *
 * The shared mechanics (agent/session setup, event serialization, the session
 * header and transcript flush) live in src/app/mode.c; this file only owns the
 * one-shot JSON entry point. See app/mode.h for the AgcModeIo contract.
 */
#include "app/mode.h"

int agentc_mode_json_run(AgcModeCtx *c, const char *prompt) {
    if (!c || !c->agent || !c->io || !c->io->write) return -22;
    /* JSON mode always speaks its own event schema, even when -p also set print
     * mode on the CLI (main then supplies on_event for the print path). */
    agentc_agent_set_events(c->agent, agentc_mode_event, c);
    agentc_mode_write_session_header(c);
    if (!prompt || !prompt[0]) return 0;
    return agentc_agent_submit(c->agent, prompt);
}
