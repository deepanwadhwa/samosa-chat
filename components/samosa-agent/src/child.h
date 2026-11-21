#ifndef SAMOSA_AGENT_CHILD_H
#define SAMOSA_AGENT_CHILD_H
#include "agent.h"
/* Public dependency operation adapter only; never opens dependency-owned state. */
const char *agent_child_id(const AgentConfig *,unsigned,char [80]);
const char *agent_child_call(const AgentConfig *,const char *,const char *,const char *,size_t,char **,size_t *);
#endif
