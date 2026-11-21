#ifndef SAMOSA_AGENT_H
#define SAMOSA_AGENT_H
#include <stdio.h>
#include <stddef.h>
#define AGENT_REQUEST_MAX 16384u
typedef struct { const char *sources_command, *inference_command, *state, *owner;
    const char *action_root,*action_host_id;
    /* Per-dispatch host/root-run control; never a process-global service slot. */
    int (*cancelled)(void *);void *cancellation;
    const char *parent_operation_id;
    const char *(*child_observer)(void *,const char *,const char *,const char *,const char *);
    void *child_observer_context;
    double deadline_monotonic,received_monotonic;
} AgentConfig;
int agent_dispatch(const AgentConfig *,const char *,size_t,FILE *);
/* Store reuses the one question policy with already admitted evidence/history. */
int agent_replay(const AgentConfig *,const char *,size_t,const char *,const char *,FILE *);
int agent_replay_session(const AgentConfig *,const char *,size_t,const char *,const char *,const char *,FILE *);
#endif
