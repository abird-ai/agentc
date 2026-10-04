/* messages.c — transcript, messages and content blocks.
 *
 * Ownership: every string in a message/block is heap-owned (agentc_alloc) and
 * released by agentc_msg_free / agentc_transcript_free. The transcript borrows the
 * model and provider names, but owns its system prompt snapshot.
 */
#include "agent.h"
#include "plat.h"

void agentc_transcript_init(AgcTranscript *t) { agentc_memset(t, 0, sizeof *t); }

static void msg_free_blocks(AgcMsg *m) {
    for (size_t i = 0; i < m->nblocks; i++) {
        AgcBlock *b = &m->blocks[i];
        agentc_free(b->text);
        agentc_free(b->tool_id);
        agentc_free(b->tool_name);
        agentc_free(b->tool_args);
    }
    agentc_free(m->blocks);
    m->blocks = NULL;
    m->nblocks = 0;
    m->blocks_cap = 0;
}

void agentc_msg_free(AgcMsg *m) {
    if (!m) return;
    msg_free_blocks(m);
    agentc_free(m->error);
    agentc_memset(m, 0, sizeof *m);
}

/* Rebuild support for the message_end override: release every block and its
 * owned strings while keeping role/usage/stop_reason/error/ts intact. */
void agentc_msg_clear_blocks(AgcMsg *m) {
    if (!m) return;
    msg_free_blocks(m);
}

void agentc_transcript_free(AgcTranscript *t) {
    if (!t) return;
    for (size_t i = 0; i < t->n; i++) agentc_msg_free(&t->msgs[i]);
    agentc_free(t->msgs);
    agentc_free((void *)t->system);
    agentc_memset(t, 0, sizeof *t);
}

AgcMsg *agentc_transcript_push(AgcTranscript *t, int role) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        t->msgs = agentc_realloc(t->msgs, cap * sizeof(AgcMsg));
        t->cap = cap;
    }
    AgcMsg *m = &t->msgs[t->n++];
    agentc_memset(m, 0, sizeof *m);
    m->role = role;
    m->stop_reason = AGENTC_STOP_PENDING;
    m->ts_ms = (u64)(os_now_ns(OS_CLOCK_REALTIME) / 1000000);
    return m;
}

/* --------------------------------------------------------------- blocks */

AgcBlock *agentc_msg_block_new(AgcMsg *m, int type) {
    if (m->nblocks == m->blocks_cap) {
        size_t cap = m->blocks_cap ? m->blocks_cap * 2 : 4;
        m->blocks = agentc_realloc(m->blocks, cap * sizeof(AgcBlock));
        m->blocks_cap = cap;
    }
    AgcBlock *b = &m->blocks[m->nblocks++];
    agentc_memset(b, 0, sizeof *b);
    b->type = type;
    return b;
}

static void str_append(char **dst, size_t *len, const char *p, size_t n) {
    if (n == 0) {
        if (!*dst) *dst = agentc_strdup_len("", 0);
        return;
    }
    size_t old = *len;
    *dst = agentc_realloc(*dst, old + n + 1);
    agentc_memcpy(*dst + old, p, n);
    (*dst)[old + n] = 0;
    *len = old + n;
}

void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n) {
    if (!b) return;
    if (b->type == AGENTC_BLK_TOOLCALL) {
        size_t old = b->tool_args ? agentc_strlen(b->tool_args) : 0;
        str_append(&b->tool_args, &old, p, n);
    } else {
        str_append(&b->text, &b->text_len, p, n);
    }
}

void agentc_msg_add_text(AgcMsg *m, const char *text, size_t n) {
    AgcBlock *b = agentc_msg_block_new(m, AGENTC_BLK_TEXT);
    agentc_msg_block_append(b, text, n);
}

void agentc_msg_add_think(AgcMsg *m) { (void)agentc_msg_block_new(m, AGENTC_BLK_THINK); }

void agentc_msg_add_tool_call(AgcMsg *m, const char *id, const char *name) {
    AgcBlock *b = agentc_msg_block_new(m, AGENTC_BLK_TOOLCALL);
    b->tool_id = agentc_strdup(id ? id : "");
    b->tool_name = agentc_strdup(name ? name : "");
}

void agentc_msg_tool_args_append(AgcMsg *m, const char *p, size_t n) {
    AgcBlock *b = NULL;
    for (size_t i = m->nblocks; i > 0; i--) {
        if (m->blocks[i - 1].type == AGENTC_BLK_TOOLCALL) {
            b = &m->blocks[i - 1];
            break;
        }
    }
    if (!b) b = agentc_msg_block_new(m, AGENTC_BLK_TOOLCALL);
    agentc_msg_block_append(b, p, n);
}

/* ------------------------------------------------------- tool results */
/* A tool-result message is AGENTC_ROLE_TOOL with one AGENTC_BLK_TEXT block whose
 * tool_id carries the provider call id and whose text carries the output. */
void agentc_msg_add_tool_result(AgcMsg *m, const char *call_id, const char *name,
                            const char *result) {
    AgcBlock *b = agentc_msg_block_new(m, AGENTC_BLK_TEXT);
    agentc_msg_block_append(b, result, agentc_strlen(result));
    b->tool_id = agentc_strdup(call_id ? call_id : "");
    b->tool_name = agentc_strdup(name ? name : "");
}

size_t agentc_msg_count_tool_calls(const AgcMsg *m) {
    size_t n = 0;
    for (size_t i = 0; i < m->nblocks; i++)
        if (m->blocks[i].type == AGENTC_BLK_TOOLCALL) n++;
    return n;
}

AgcBlock *agentc_msg_nth_tool_call(AgcMsg *m, size_t n) {
    for (size_t i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].type == AGENTC_BLK_TOOLCALL) {
            if (n == 0) return &m->blocks[i];
            n--;
        }
    }
    return NULL;
}
