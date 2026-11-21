#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "source_call.h"
#include "budget.h"
#include "command.h"
#include "json.h"
#include <math.h>
#include <time.h>
#include <unistd.h>
static unsigned checks,events;static char status[32],outcome[32];
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL shared budget %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const char *observe(void *ctx,const char *id,const char *kind,const char *state,const char *result){(void)ctx;CHECK(!strncmp(id,"agent-child-",12));CHECK(!strcmp(kind,"source_read_text"));events++;snprintf(status,sizeof(status),"%s",state);snprintf(outcome,sizeof(outcome),"%s",result);return NULL;}
static int provider(const char *mode){char bytes[16384];size_t n=fread(bytes,1,sizeof(bytes),stdin);Json j;if(!json_parse(&j,bytes,n))return 2;char *op=json_string(&j,json_get(&j,0,"op"));if(!op)return 2;
    if(!strcmp(op,"describe")){fprintf(stdout,"{\"contract_version\":1,\"capabilities\":[%s]}",!strcmp(mode,"unsupported")?"":"\"request_budgets\",\"request_correlation\"");free(op);return 0;}free(op);
    if(!strcmp(mode,"slow")){struct timespec delay={10,0};nanosleep(&delay,NULL);}
    char *id=json_string(&j,json_get(&j,0,"operation_id")),*parent=json_string(&j,json_get(&j,0,"parent_operation_id"));size_t ms;
    if(!id||!parent||!json_uint(&j,json_get(&j,0,"timeout_ms"),&ms)||!ms||ms>30000){free(id);free(parent);return 2;}
    fputs("{\"contract_version\":1,",stdout);if(!strcmp(mode,"failed"))fputs("\"error\":{\"code\":\"deadline_exceeded\"}",stdout);else fputs("\"text\":\"stable\"",stdout);
    fputs(",\"operation_id\":",stdout);json_quote(stdout,!strcmp(mode,"forged")?"wrong":id,strlen(!strcmp(mode,"forged")?"wrong":id));fputs(",\"parent_operation_id\":",stdout);json_quote(stdout,parent,strlen(parent));fputc('}',stdout);free(id);free(parent);return 0;}
static char *command(const char *self,const char *mode){char *s=NULL;size_t n;FILE *f=open_memstream(&s,&n);CHECK(f);fputc('[',f);json_quote(f,self,strlen(self));fputs(",\"--provider\",",f);json_quote(f,mode,strlen(mode));fputc(']',f);CHECK(!fclose(f));return s;}
int main(int argc,char **argv){int guarded=command_guardian_dispatch(argc,argv);if(guarded>=0)return guarded;if(argc==3&&!strcmp(argv[1],"--provider"))return provider(argv[2]);
    char self[4096];CHECK(realpath(argv[0],self));AgentConfig cfg={.parent_operation_id="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.child_observer=observe};
    static const char request[]="{\"contract_version\":1,\"op\":\"read_text\"}";
    const char *modes[]={"normal","forged","failed","unsupported","slow"};
    for(unsigned i=0;i<5;i++){char *cmd=command(self,modes[i]);cfg.sources_command=cmd;cfg.deadline_monotonic=agent_now()+(i==4?5:10);unsigned index=0;events=0;char *response=NULL;size_t n=0;double before=agent_now();
        const char *error=agent_source_call(&cfg,&index,"source_read_text",request,sizeof(request)-1,&response,&n);
        if(i==0){CHECK(!error);CHECK(!strcmp(response,"{\"contract_version\":1,\"text\":\"stable\"}"));CHECK(events==2&&!strcmp(status,"completed")&&!strcmp(outcome,"not_requested"));}
        if(i==1){CHECK(error&&!strcmp(error,"invalid_child_operation"));CHECK(events==2&&!strcmp(status,"unconfirmed"));}
        if(i==2){CHECK(!error);CHECK(strstr(response,"deadline_exceeded"));CHECK(!strstr(response,"operation_id"));CHECK(events==2&&!strcmp(status,"failed"));}
        if(i==3){CHECK(error&&!strcmp(error,"unsupported_capability"));CHECK(!events&&!index);}
        if(i==4){CHECK(error&&!strcmp(error,"deadline_exceeded"));CHECK(agent_now()-before<8);CHECK(events==2&&!strcmp(status,"unconfirmed")&&!strcmp(outcome,"unconfirmed"));}
        free(response);free(cmd);
    }
    cfg.deadline_monotonic=NAN;CHECK(!strcmp(agent_interrupted(&cfg),"invalid_configuration"));cfg.deadline_monotonic=agent_now()-1;CHECK(!strcmp(agent_interrupted(&cfg),"deadline_exceeded"));CHECK(!agent_remaining_ms(&cfg));
    printf("PASS shared budget adapter: %u checks; correlated Source evidence, refusal, expiry and honest child outcomes\n",checks);return 0;}
