#ifndef SAMOSA_AGENT_EVIDENCE_H
#define SAMOSA_AGENT_EVIDENCE_H
#include <stddef.h>
/* Shared question/finding validation of public literal envelopes. Identity is
 * allocated only on success; caller owns it. No model or filesystem access. */
const char *agent_evidence_validate(const char *,size_t,const char *,const char *,size_t,size_t,int,char **);
#endif
