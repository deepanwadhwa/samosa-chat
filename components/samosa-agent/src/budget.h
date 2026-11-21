#ifndef SAMOSA_AGENT_BUDGET_H
#define SAMOSA_AGENT_BUDGET_H
#include "agent.h"
double agent_now(void);
const char *agent_interrupted(const AgentConfig *);
double agent_call_timeout(const AgentConfig *);
size_t agent_remaining_ms(const AgentConfig *);
const char *agent_call(const AgentConfig *,const char *,const char *,size_t,char **,size_t *);
#endif
