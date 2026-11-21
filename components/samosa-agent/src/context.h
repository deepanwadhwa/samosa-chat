#ifndef SAMOSA_AGENT_CONTEXT_H
#define SAMOSA_AGENT_CONTEXT_H
#include <stddef.h>
/* Locally owned semantic context helper; never source acquisition/model policy. */
const char *context_history(const char *,size_t *);
char *context_slice(const char *,size_t);
char *context_prompt(const char *,const char *,const char *);
#endif
