/* markdown.h — streaming-safe markdown block renderer.
 *
 * Text is appended incrementally; only the last (incomplete) block is
 * re-rendered per delta. Completed blocks keep their wrapped, styled rows.
 */
#ifndef AGENTC_TUI_MARKDOWN_H
#define AGENTC_TUI_MARKDOWN_H

#include "agentc.h"
#include "render.h"

enum {
    MD_PARA = 0,
    MD_HEAD,
    MD_BULLET,
    MD_CODE,
};

typedef struct {
    u16 start, len;   /* byte range into MdRow.line */
    u16 attrs;
    u16 fg;           /* theme slot; 0xFFFF = inherit block color */
} MdRun;

typedef struct {
    AgcBuf line;       /* UTF-8 row text */
    MdRun *runs;
    size_t nruns, runs_cap;
} MdRow;

typedef struct {
    size_t start, end;     /* byte range in Markdown.text */
    bool complete;
    int kind;
    MdRow *rows;
    size_t nrows, rows_cap;
} MdBlock;

typedef struct {
    AgcBuf text;
    MdBlock *blocks;
    size_t nblocks, blocks_cap;
    int width;
    u16 base_fg;
    u16 base_attrs;
} Markdown;

void md_init(Markdown *m, u16 base_fg, u16 base_attrs);
void md_free(Markdown *m);
void md_reset(Markdown *m);
void md_set_width(Markdown *m, int width);
void md_append(Markdown *m, const char *p, size_t n);
size_t md_height(const Markdown *m);
const MdBlock *md_blocks(const Markdown *m, size_t *nblocks);

#endif /* AGENTC_TUI_MARKDOWN_H */
