/* compact.c — context estimation and transcript compaction helpers.
 *
 * The agent loop (agent.c) owns the summarization request; this file provides
 * the pure pieces:
 *   - agentc_compact_estimate(): last assistant usage + chars/4 of trailing messages
 *   - agentc_compact_cut():      walk back from the tail accumulating
 *                            keep_recent_tokens, stopping only at a boundary
 *                            that does not split a tool call from its result
 *   - agentc_compact_system_prompt(): the checkpoint instructions
 */
#include "agent.h"

/* Roughly one token per 4 bytes; block overhead keeps tiny messages from
 * under-counting. */
static size_t msg_chars(const AgcMsg *m) {
    size_t n = 8;
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *b = &m->blocks[i];
        if (b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK) n += b->text_len;
        if (b->type == AGENTC_BLK_TOOLCALL) {
            if (b->tool_args) n += agentc_strlen(b->tool_args);
            if (b->tool_name) n += agentc_strlen(b->tool_name);
        }
    }
    return n;
}

u32 agentc_compact_estimate(const AgcTranscript *tr) {
    if (!tr) return 0;
    u64 est = 0;
    size_t start = 0;
    bool found = false;
    for (size_t i = tr->n; i > 0; i--) {
        const AgcMsg *m = &tr->msgs[i - 1];
        if (m->role == AGENTC_ROLE_ASSISTANT && (m->usage.input || m->usage.output)) {
            est = (u64)m->usage.input + m->usage.output;
            start = i;
            found = true;
            break;
        }
    }
    if (!found && tr->system) est = agentc_strlen(tr->system) / 4;
    for (size_t i = start; i < tr->n; i++) est += (msg_chars(&tr->msgs[i]) + 3) / 4;
    if (est > 0xFFFFFFFFu) est = 0xFFFFFFFFu;
    return (u32)est;
}

size_t agentc_compact_cut(const AgcTranscript *tr, u32 keep_recent_tokens) {
    if (!tr || tr->n == 0) return 0;
    u64 acc = 0;
    size_t c = tr->n;
    while (c > 0) {
        u64 need = (msg_chars(&tr->msgs[c - 1]) + 3) / 4;
        if (acc + need > keep_recent_tokens) break;
        acc += need;
        c--;
    }
    /* The kept region must not begin with a tool result: its assistant call
     * would be compacted away. Step back to include the call. */
    while (c > 0 && c < tr->n && tr->msgs[c].role == AGENTC_ROLE_TOOL) c--;
    return c;
}

const char *agentc_compact_system_prompt(void) {
    return
        "You are compacting a coding-agent conversation for another agent that "
        "will continue the work. Summarize the conversation as a checkpoint with "
        "exactly these sections:\n"
        "## Goal\n"
        "## Constraints\n"
        "## Progress\n"
        "## Decisions\n"
        "## Next Steps\n"
        "## Critical Context\n"
        "Be concrete: keep file paths, commands, error messages, partial results "
        "and open questions. Do not invent new tasks.";
}
