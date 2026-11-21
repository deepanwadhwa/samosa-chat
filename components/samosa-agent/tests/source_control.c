#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "agent.h"
#include "command.h"
#include "json.h"
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL Source integration %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+(double)t.tv_nsec/1e9;}
static void delay(void){struct timespec t={0,10000000};nanosleep(&t,NULL);}
static char *args(const char **v,size_t count){char *s=NULL;size_t n;FILE *f=open_memstream(&s,&n);CHECK(f);fputc('[',f);for(size_t i=0;i<count;i++){if(i)fputc(',',f);json_quote(f,v[i],strlen(v[i]));}fputc(']',f);CHECK(!fclose(f));return s;}
static char *external(const char *cmd,const char *req){char *out=NULL;size_t n;double started=now();const char *e=command_call(cmd,req,strlen(req),20,&out,&n);if(e)fprintf(stderr,"External transport: %s after %.3fs\nRequest: %s\n",e,now()-started,req);CHECK(!e);if(!out||strstr(out,"\"error\""))fprintf(stderr,"External request: %s\nReply: %s\n",req,out?out:"(empty)");CHECK(out&&!strstr(out,"\"error\""));return out;}
static char *call(const AgentConfig *cfg,const char *req,int rc){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);int got=agent_dispatch(cfg,req,strlen(req),f);CHECK(!fclose(f));if(got!=rc)fprintf(stderr,"Agent: %s\n",out);CHECK(got==rc);return out;}
static char *field(const char *s,const char *key){Json j;CHECK(json_parse(&j,s,strlen(s)));char *out=json_string(&j,json_get(&j,0,key));CHECK(out);return out;}
static char *value(const char *s,const char *key){Json j;CHECK(json_parse(&j,s,strlen(s)));int t=json_get(&j,0,key);CHECK(t>=0);char *out=strndup(s+j.tokens[t].begin,j.tokens[t].end-j.tokens[t].begin);CHECK(out);return out;}
static char *read_file(const char *path){FILE *f=fopen(path,"r");CHECK(f);char *out=malloc(65537);CHECK(out);size_t n=fread(out,1,65536,f);CHECK(n&&!ferror(f));out[n]=0;CHECK(!fclose(f));return out;}
static int has_status(const char *reply,const char *expected){char *s=field(reply,"status");int ok=!strcmp(s,expected);free(s);return ok;}
static void await_worker(pid_t p,double timeout){double end=now()+timeout;for(;;){int st;pid_t got=waitpid(p,&st,WNOHANG);CHECK(got>=0);if(got==p){CHECK(WIFEXITED(st)&&!WEXITSTATUS(st));return;}CHECK(now()<end);delay();}}
static void request(char *s,size_t n,const char *conversation,const char *source,unsigned revision,const char *id,unsigned page,unsigned budget){CHECK(snprintf(s,n,"{\"contract_version\":1,\"op\":\"submit\",\"conversation_id\":\"%s\",\"expected_revision\":%u,\"operation_id\":\"%s\",\"question\":\"What is the code?\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":%u,\"end_page\":%u,\"timeout_ms\":%u}",conversation,revision,id,source,source,page,page+3,budget)>0);}
int main(int argc,char **argv){int guarded=command_guardian_dispatch(argc,argv);if(guarded>=0)return guarded;
    if(argc==7&&!strcmp(argv[1],"--worker")){AgentConfig cfg={.state=argv[2],.owner="alice",.sources_command=argv[3],.inference_command=argv[4]};FILE *f=fopen(argv[6],"w");if(!f)return 2;int rc=agent_dispatch(&cfg,argv[5],strlen(argv[5]),f);if(fclose(f))rc=2;return rc;}
    CHECK(argc==6||(argc==7&&!strcmp(argv[6],"--allow-unconfirmed")));int allow_unconfirmed=argc==7;unsigned unconfirmed=0;char self[4096];CHECK(realpath(argv[0],self));char root[]="/tmp/samosa-source-agent.XXXXXX";CHECK(mkdtemp(root));
    char state[1024],source_state[1024],pdf[1024],log[1024],result[1024],req[2048];snprintf(state,sizeof(state),"%s/agent",root);snprintf(source_state,sizeof(source_state),"%s/sources",root);snprintf(pdf,sizeof(pdf),"%s/fixture.pdf",root);snprintf(log,sizeof(log),"%s/pages",root);snprintf(result,sizeof(result),"%s/result",root);
    char *generate_args[]={argv[3],pdf,NULL};pid_t generator;CHECK(!posix_spawnp(&generator,argv[3],NULL,NULL,generate_args,environ));await_worker(generator,8);
    const char *source_argv[]={argv[1],"--state",source_state,"--root",root,"--owner","alice","--pdfium-library",argv[4]};char *source_cmd=args(source_argv,9);
    char *out=external(source_cmd,"{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":\"fixture.pdf\"}");char *source=field(out,"source_id");free(out);
    char admission[1024];snprintf(admission,sizeof(admission),"%s/inference",root);
    const char *infer_argv[]={argv[2],"--admission-state",admission,"--owner","alice"};char *infer=args(infer_argv,5);AgentConfig cfg={.state=state,.owner="alice",.sources_command=source_cmd,.inference_command=infer};out=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}",0);char *conversation=field(out,"conversation_id");free(out);
    request(req,sizeof(req),conversation,source,0,"initial",1,15000);out=call(&cfg,req,0);if(!has_status(out,"completed"))fprintf(stderr,"Initial: %s\n",out);CHECK(has_status(out,"completed"));free(out);
    char get[256];snprintf(get,sizeof(get),"{\"contract_version\":1,\"op\":\"get\",\"conversation_id\":\"%s\"}",conversation);out=call(&cfg,get,0);char *evidence=value(out,"admitted_evidence"),*scope=value(out,"selected_scope"),*continuation=value(out,"continuation");CHECK(strcmp(evidence,"null"));free(out);
    CHECK(!setenv("SAMOSA_TEST_REAL_PDFIUM",argv[4],1));CHECK(!setenv("SAMOSA_TEST_PDF_SPY_LOG",log,1));CHECK(!setenv("SAMOSA_TEST_PDF_PAGE_DELAY_MS","2000",1));source_argv[8]=argv[5];char *slow=args(source_argv,9);cfg.sources_command=slow;
    for(unsigned i=0;i<2;i++){
        if(i)CHECK(!unlink(log));
        request(req,sizeof(req),conversation,source,2+i,i?"deadline":"cancel",2+i,i?4000:15000);
        char *worker_argv[]={self,"--worker",state,slow,infer,req,result,NULL};pid_t p;double started=now();CHECK(!posix_spawnp(&p,self,NULL,NULL,worker_argv,environ));
        if(!i){double end=now()+20;struct stat st;while(stat(log,&st)||!st.st_size){int status;CHECK(waitpid(p,&status,WNOHANG)==0);CHECK(now()<end);delay();}
            char peer[512];snprintf(peer,sizeof(peer),"{\"contract_version\":1,\"op\":\"read_pdf\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":97,\"end_page\":97,\"operation_id\":\"unrelated-reader\",\"parent_operation_id\":\"unrelated-parent\",\"timeout_ms\":10000}",source,source);out=external(source_cmd,peer);CHECK(strstr(out,"7319"));free(out);
            AgentConfig foreign=cfg;foreign.owner="bob";out=call(&foreign,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"cancel\"}",1);CHECK(strstr(out,"unknown_run"));free(out);
            out=call(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"cancel\"}",0);CHECK(strstr(out,"\"cancel_requested\":true"));free(out);
        }
        await_worker(p,allow_unconfirmed?12:8);CHECK(now()-started<25);out=read_file(result);if(i)CHECK(strstr(out,"deadline_exceeded"));CHECK(has_status(out,i?"failed":"cancelled"));
        Json j;CHECK(json_parse(&j,out,strlen(out)));int children=json_get(&j,0,"children");CHECK(children>=0&&j.tokens[children].count==1);char *child=json_string(&j,json_get(&j,children+1,"operation_id")),*observed=json_string(&j,json_get(&j,children+1,"status")),*outcome=json_string(&j,json_get(&j,children+1,"cancellation"));CHECK(child&&observed&&outcome);char control[256];snprintf(control,sizeof(control),"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"%s\"}",child);char *terminal=external(source_cmd,control);
        if(!strcmp(observed,"unconfirmed")){CHECK(allow_unconfirmed);CHECK(!strcmp(outcome,"unconfirmed"));CHECK(has_status(terminal,"interrupted")||has_status(terminal,"cancelled")||has_status(terminal,"failed")||has_status(terminal,"completed"));unconfirmed++;}
        else{CHECK(has_status(terminal,observed));CHECK(!strcmp(observed,"cancelled")||(i&&!strcmp(observed,"failed")));if(!i)CHECK(!strcmp(outcome,"observed_cancelled"));}
        char *parent=field(out,"run_id"),*source_parent=field(terminal,"parent_operation_id");CHECK(!strcmp(parent,source_parent));free(parent);free(source_parent);free(terminal);free(child);free(observed);free(outcome);
        AgentConfig offline=cfg;offline.sources_command=NULL;offline.inference_command=NULL;char *retry=call(&offline,req,0);CHECK(!strcmp(out,retry));free(out);free(retry);
        out=call(&offline,get,0);char *current=value(out,"admitted_evidence");CHECK(!strcmp(evidence,current));free(current);current=value(out,"selected_scope");CHECK(!strcmp(scope,current));free(current);current=value(out,"continuation");CHECK(!strcmp(continuation,current));free(current);CHECK(strstr(out,"Fake answer; native contract test only."));free(out);
        terminal=external(source_cmd,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"unrelated-reader\"}");CHECK(has_status(terminal,"completed"));CHECK(strstr(terminal,"\"cancel_requested\":false"));free(terminal);
    }
    cfg.sources_command=source_cmd;request(req,sizeof(req),conversation,source,4,"survivor",4,15000);out=call(&cfg,req,0);CHECK(has_status(out,"completed"));free(out);
    free(evidence);free(scope);free(continuation);free(conversation);free(source);free(source_cmd);free(slow);free(infer);printf("PASS Source/Agent native control: %u checks; scoped cancellation, public terminal, deadline, offline retry, last-good preservation, concurrent peer and survivor; unconfirmed allowed=%d observed=%u; QA %s\n",checks,allow_unconfirmed,unconfirmed,root);return 0;
}
