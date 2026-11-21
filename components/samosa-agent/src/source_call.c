#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_call.h"
#include "budget.h"
#include "child.h"
#include "json.h"
static int envelope(Json *j,const char *bytes,size_t n){size_t v;return json_parse(j,bytes,n)&&j->tokens[0].type==JT_OBJECT&&json_uint(j,json_get(j,0,"contract_version"),&v)&&v==1;}
const char *agent_source_call(const AgentConfig *cfg,unsigned *index,const char *kind,const char *request,size_t n,char **response,size_t *length){
    if(!cfg->deadline_monotonic)return agent_call(cfg,cfg->sources_command,request,n,response,length);
    static const char describe[]="{\"contract_version\":1,\"op\":\"describe\"}";
    char *descriptor=NULL;size_t dn=0;Json j;int correlation=0,budget=0,operations=0,cancellation=0,parents=0;
    const char *error=agent_call(cfg,cfg->sources_command,describe,sizeof(describe)-1,&descriptor,&dn);
    if(!error&&envelope(&j,descriptor,dn)&&json_get(&j,0,"error")<0){
        int caps=json_get(&j,0,"capabilities");
        if(caps>=0&&j.tokens[caps].type==JT_ARRAY)for(int t=caps+1;t<j.tokens[caps].next;t=j.tokens[t].next){char *s=json_string(&j,t);if(s){if(!strcmp(s,"request_correlation"))correlation=1;if(!strcmp(s,"request_budgets"))budget=1;if(!strcmp(s,"operations"))operations=1;if(!strcmp(s,"cancellation"))cancellation=1;if(!strcmp(s,"operation_parents"))parents=1;}free(s);}
    }
    free(descriptor);if(error)return error;if(!correlation||!budget)return "unsupported_capability";
    error=agent_interrupted(cfg);if(error)return error;
    char id[80];error=agent_child_id(cfg,++*index,id);if(error)return error;
    size_t remaining=agent_remaining_ms(cfg);if(!remaining)return "deadline_exceeded";
    char *body=NULL;size_t bn=0;FILE *f=open_memstream(&body,&bn);if(!f)return "resource_limit";
    fwrite(request,1,n-1,f);fprintf(f,",\"timeout_ms\":%zu,\"operation_id\":\"%s\",\"parent_operation_id\":\"%s\"}",remaining,id,cfg->parent_operation_id);
    if(fclose(f)){free(body);return "resource_limit";}
    if(operations&&cancellation&&parents){
        AgentConfig dependency=*cfg;dependency.inference_command=cfg->sources_command;
        error=agent_child_call(&dependency,id,kind,body,bn,response,length);
        if(!error&&!envelope(&j,*response,*length))error="invalid_evidence";
    }else{
        if(cfg->child_observer)error=cfg->child_observer(cfg->child_observer_context,id,kind,"running","not_requested");
        if(!error){
            error=agent_call(cfg,cfg->sources_command,body,bn,response,length);
            const char *status="unconfirmed",*outcome="unconfirmed";
            if(!error){
                if(!envelope(&j,*response,*length))error="invalid_evidence";
                else{
                    char *child=json_string(&j,json_get(&j,0,"operation_id")),*parent=json_string(&j,json_get(&j,0,"parent_operation_id"));
                    if(!child||!parent||strcmp(child,id)||strcmp(parent,cfg->parent_operation_id))error="invalid_child_operation";
                    else{status=json_get(&j,0,"error")>=0?"failed":"completed";outcome="not_requested";}
                    free(child);free(parent);
                }
            }
            if(cfg->child_observer){const char *observed=cfg->child_observer(cfg->child_observer_context,id,kind,status,outcome);if(!error)error=observed;}
        }
    }
    {
        /* Correlation is transport metadata, never part of admitted evidence. */
        if(!error){
            char *clean=NULL;size_t cn=0;f=open_memstream(&clean,&cn);if(!f)error="resource_limit";
            else{int comma=0;fputc('{',f);for(int t=1;t<j.tokens[0].next;){int value=j.tokens[t].next;char *key=json_string(&j,t);
                if(key&&strcmp(key,"operation_id")&&strcmp(key,"parent_operation_id")){if(comma++)fputc(',',f);fwrite(j.text+j.tokens[t].begin,1,j.tokens[value].end-j.tokens[t].begin,f);}free(key);t=j.tokens[value].next;}
                fputc('}',f);if(fclose(f)){free(clean);error="resource_limit";}else{free(*response);*response=clean;*length=cn;}}
        }
    }
    free(body);return error;
}
