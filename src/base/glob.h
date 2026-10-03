/* glob.h — the bounded glob matcher shared by find, grep and .gitignore.
 *
 * One matcher for every glob in the tree: `*` (not crossing `/`), `**`
 * (crossing), a `**` directory prefix may match zero directories, `?` (not
 * crossing `/`), `[abc]`/`[^abc]` classes with `\` escapes and literal bytes
 * otherwise.
 * Patterns longer than AGENTC_GLOB_MAX_PATTERN and matches that need more than
 * AGENTC_GLOB_STEP_BUDGET steps report "no match" instead of hanging; the
 * matcher is iterative (no recursion), so a hostile pattern cannot exhaust the
 * stack either. Only agentc.h is needed.
 */
#ifndef AGENTC_BASE_GLOB_H
#define AGENTC_BASE_GLOB_H

#include "agentc.h"

#define AGENTC_GLOB_MAX_PATTERN 256u       /* bytes; longer patterns never match */
#define AGENTC_GLOB_STEP_BUDGET 1000000u   /* transitions per agentc_glob_match() */

/* NULL pattern or text is "no match"; text is NUL-terminated. */
bool agentc_glob_match(const char *pattern, const char *text);

#endif /* AGENTC_BASE_GLOB_H */
