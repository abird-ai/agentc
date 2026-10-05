/* stdio.h — no stdio exists in this build; the vendored TLS only needs these
 * declarations (and agentc supplies the implementations). Keeping the header
 * here stops a host <stdio.h> from being pulled in through a library header. */
#ifndef AGENTC_FREESTANDING_STDIO_H
#define AGENTC_FREESTANDING_STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct agentc_file FILE;

int snprintf(char *buf, size_t cap, const char *fmt, ...);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

#endif
