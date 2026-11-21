#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "child.h"
#include "command.h"
#include "json.h"
#include "sha256.h"
#include "budget.h"
#include <time.h>
typedef struct {const AgentConfig *cfg;const char *id;int requested;const char *outcome,*terminal,*reason;double deadline;} ChildControl;
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int envelope(const char *bytes,size_t length,const ChildControl *c,Json *j) {
    size_t version;
    if(!json_parse(j,bytes,length)||j->tokens[0].type!=JT_OBJECT||!json_uint(j,json_get(j,0,"contract_version"),&version)||version!=1)return 0;
    char *id=json_string(j,json_get(j,0,"operation_id")),*parent=json_string(j,json_get(j,0,"parent_operation_id"));
    int valid=id&&parent&&!strcmp(id,c->id)&&!strcmp(parent,c->cfg->parent_operation_id);free(id);free(parent);return valid;
}
static const char *event(const ChildControl *c,const char *op,const char *status,const char *outcome) {
    return c->cfg->child_observer?c->cfg->child_observer(c->cfg->child_observer_context,c->id,op,status,outcome):NULL;
}
static void cancel_provider(ChildControl *c) {
    c->requested=1;c->outcome="unconfirmed";double started=now();if(started<0)return;int accepted=0;
    for(unsigned attempt=0;attempt<60;attempt++) {
        double current=now();if(current<0||current-started>=3)return;
        char request[256];int n=snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"%s\",\"operation_id\":\"%s\"}",accepted?"operation_get":"operation_cancel",c->id);
        char *response=NULL;size_t length=0;
        double remaining=3-(current-started);if(remaining>1.5)remaining=1.5;
        const char *error=command_call(c->cfg->inference_command,request,(size_t)n,remaining,&response,&length);Json j;
#ifdef SAMOSA_AGENT_TEST_FAULTS
        if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR"))
            fprintf(stderr,"QA child control: attempt=%u elapsed=%.3f error=%s response=%.*s\n",attempt,now()-started,error?error:"none",(int)(length<1024?length:1024),response?response:"");
#endif
        if(!error&&envelope(response,length,c,&j)&&json_get(&j,0,"error")<0) {
            int flag=json_get(&j,0,"cancel_requested");
            if(flag>=0&&j.tokens[flag].type==JT_BOOL&&j.tokens[flag].end-j.tokens[flag].begin==4&&!memcmp(response+j.tokens[flag].begin,"true",4))accepted=1;
            char *status=json_string(&j,json_get(&j,0,"status"));
            if(status) {
                if(!strcmp(status,"cancelled")){c->outcome="observed_cancelled";c->terminal="cancelled";}
                else if(!strcmp(status,"completed")){c->outcome="observed_completed";c->terminal="completed";}
                else if(!strcmp(status,"failed")){c->outcome="observed_failed";c->terminal="failed";}
                else if(!strcmp(status,"interrupted")){c->outcome="observed_interrupted";c->terminal="interrupted";}
                free(status);
            }
        }
        free(response);if(c->terminal)return;
        struct timespec pause={0,50000000};nanosleep(&pause,NULL);
    }
}
static int cancelled(void *opaque) {
    ChildControl *c=opaque;
    c->reason=agent_interrupted(c->cfg);
    if(!c->reason&&now()>=c->deadline)c->reason="deadline_exceeded";
    if(!c->reason)return 0;
    if(!c->requested)cancel_provider(c);
    return 1;
}
const char *agent_child_id(const AgentConfig *cfg,unsigned index,char out[80]) {
    if(!cfg||!cfg->parent_operation_id||!index||index>16||strlen(cfg->parent_operation_id)!=64)return "invalid_child_operation";
    for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",cfg->parent_operation_id[i]))return "invalid_child_operation";
    char binding[96],sha[65];int n=snprintf(binding,sizeof(binding),"%s:%u",cfg->parent_operation_id,index);hash_bytes(binding,(size_t)n,sha);
    snprintf(out,80,"agent-child-%s",sha);return NULL;
}
const char *agent_child_call(const AgentConfig *cfg,const char *id,const char *op,const char *request,size_t length,char **response,size_t *response_length) {
    ChildControl c={.cfg=cfg,.id=id,.outcome="not_requested"};
    *response=NULL;*response_length=0;
    if(!cfg||!id||!*id||strlen(id)>128||!op||!cfg->parent_operation_id||!cfg->inference_command)return "invalid_child_operation";
    const char *interruption=agent_interrupted(cfg);if(interruption)return interruption;
    double timeout=agent_call_timeout(cfg);if(timeout<=0)return "deadline_exceeded";c.deadline=now()+timeout;
    const char *error=event(&c,op,"running","not_requested");if(error)return error;
    error=command_call_controlled(cfg->inference_command,request,length,timeout,response,response_length,cancelled,&c);
    if(c.reason)error=c.reason;
    const char *status=c.terminal?c.terminal:"unconfirmed";
    if(!error) {
        Json j;if(!envelope(*response,*response_length,&c,&j))error="invalid_child_response";
        else {
            int e=json_get(&j,0,"error");status=e>=0?"failed":"completed";
            char *code=e>=0?json_string(&j,json_get(&j,e,"code")):NULL;
            if(code&&!strcmp(code,"cancelled"))status="cancelled";
            free(code);
        }
    }
    const char *recorded=event(&c,op,status,c.outcome);
    return recorded?recorded:error;
}
