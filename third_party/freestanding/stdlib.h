/* stdlib.h — allocation hooks only; src/base/mem.c provides the memory. */
#ifndef AGENTC_FREESTANDING_STDLIB_H
#define AGENTC_FREESTANDING_STDLIB_H
#include <stddef.h>

void *calloc(size_t nmemb, size_t size);
void free(void *ptr);
void abort(void);

#endif
