#ifndef SAMOSA_AGENT_RECONCILE_H
#define SAMOSA_AGENT_RECONCILE_H
#include <stdint.h>
/* Returns an owned qualified result; physical effect history is preserved. */
char *action_reconcile(const char *,const char *,const char *,const char *,const char *,
                       uint64_t,uint64_t,const char *);
#endif
