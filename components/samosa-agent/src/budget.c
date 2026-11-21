#define _POSIX_C_SOURCE 200809L
#include "budget.h"
#include "command.h"
#include <math.h>
#include <time.h>
double agent_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
const char *agent_interrupted(const AgentConfig *cfg){
    if(!cfg)return NULL;
    if(!isfinite(cfg->deadline_monotonic)||cfg->deadline_monotonic<0)return "invalid_configuration";
    if(cfg->cancelled&&cfg->cancelled(cfg->cancellation))return "cancelled";
    if(cfg->deadline_monotonic){double now=agent_now();if(now<0)return "state_unavailable";if(now>=cfg->deadline_monotonic)return "deadline_exceeded";}
    return NULL;
}
double agent_call_timeout(const AgentConfig *cfg){if(!cfg||!cfg->deadline_monotonic)return 10;double remaining=cfg->deadline_monotonic-agent_now();return remaining<30?remaining:30;}
size_t agent_remaining_ms(const AgentConfig *cfg){double timeout=agent_call_timeout(cfg);if(!isfinite(timeout)||timeout<=0)return 0;size_t ms=(size_t)(timeout*1000);return ms?ms:1;}
static int interrupted(void *opaque){return agent_interrupted(opaque)!=NULL;}
const char *agent_call(const AgentConfig *cfg,const char *command,const char *request,size_t length,char **response,size_t *n){
    *response=NULL;*n=0;const char *error=agent_interrupted(cfg);if(error)return error;double timeout=agent_call_timeout(cfg);if(timeout<=0)return "deadline_exceeded";
    error=command_call_controlled(command,request,length,timeout,response,n,interrupted,(void*)cfg);
    const char *status=agent_interrupted(cfg);return status?status:error;
}
