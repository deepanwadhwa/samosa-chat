#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "agent.h"
#include "command.h"
#include "json.h"
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
static const char *self;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL concurrent %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static void tick(void){struct timespec t={0,10000000};nanosleep(&t,NULL);}
static char *call(const AgentConfig *cfg,const char *request,int expected){char *out=NULL;size_t n=0;FILE *f=open_memstream(&out,&n);CHECK(f);int rc=agent_dispatch(cfg,request,strlen(request),f);CHECK(!fclose(f));if(rc!=expected)fprintf(stderr,"response: %s\n",out);CHECK(rc==expected);Json j;CHECK(json_parse(&j,out,n));return out;}
static char *field(const char *out,const char *name){Json j;CHECK(json_parse(&j,out,strlen(out)));char *v=json_string(&j,json_get(&j,0,name));CHECK(v);return v;}
static void expect_status(const char *out,const char *status){char *v=field(out,"status");CHECK(!strcmp(v,status));free(v);}
static void expect_error(const char *out,const char *error){Json j;CHECK(json_parse(&j,out,strlen(out)));char *v=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));CHECK(v&&!strcmp(v,error));free(v);}
static void state(const AgentConfig *cfg,const char *id,size_t revision,unsigned messages){char req[256];snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"get\",\"conversation_id\":\"%s\"}",id);char *out=call(cfg,req,0);Json j;CHECK(json_parse(&j,out,strlen(out)));size_t actual;CHECK(json_uint(&j,json_get(&j,0,"revision"),&actual)&&actual==revision);int array=json_get(&j,0,"messages");CHECK(array>=0&&j.tokens[array].count==(int)messages);free(out);}
static void submit(char req[1024],const char *id,size_t revision,const char *op,unsigned index){snprintf(req,1024,"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":%zu,\"operation_id\":\"%s\",\"question\":\"hold-%u\",\"timeout_ms\":60000}",id,revision,op,index);}
static char *run(const AgentConfig *cfg,const char *op,const char *operation,int expected){char req[256];snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"%s\",\"operation_id\":\"%s\"}",op,operation);return call(cfg,req,expected);}
static void path(char out[512],const char *root,const char *kind,unsigned i){snprintf(out,512,"%s/%s-%u",root,kind,i);}
static void release(const char *root,unsigned i){char p[512];path(p,root,"release",i);int fd=open(p,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(!close(fd));}
static void reset(const char *root,unsigned i){char p[512];path(p,root,"release",i);CHECK(!unlink(p)||errno==ENOENT);path(p,root,"ready",i);CHECK(!unlink(p)||errno==ENOENT);}
static int provider(const char *root){char input[65537];size_t n=fread(input,1,sizeof(input),stdin);Json j;CHECK(json_parse(&j,input,n));char *op=json_string(&j,json_get(&j,0,"op"));CHECK(op);
    if(!strcmp(op,"describe")){fputs("{\"contract_version\":1,\"provider_id\":\"concurrent-fixture\",\"context_tokens\":8192,\"capabilities\":[\"text\",\"count_tokens\",\"request_budgets\"]}",stdout);free(op);return 0;}
    char *prompt=json_string(&j,json_get(&j,0,"prompt"));CHECK(prompt);size_t length=strlen(prompt);
    if(!strcmp(op,"count_tokens"))fprintf(stdout,"{\"contract_version\":1,\"provider_id\":\"concurrent-fixture\",\"input_tokens\":%zu,\"context_tokens\":8192}",length);
    else {CHECK(!strcmp(op,"generate"));const char *hold=strstr(prompt,"hold-");CHECK(hold);unsigned i;CHECK(sscanf(hold,"hold-%u",&i)==1&&i<9);char marker[512],gate[512];path(marker,root,"ready",i);path(gate,root,"release",i);int fd=open(marker,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(write(fd,"active",6)==6);CHECK(!close(fd));double end=now()+25;while(access(gate,F_OK)){CHECK(now()<end);tick();}
        fprintf(stdout,"{\"contract_version\":1,\"provider_id\":\"concurrent-fixture\",\"answer\":\"42\",\"usage\":{\"input_tokens\":%zu,\"output_tokens\":2}}",length);
    }free(op);free(prompt);return 0;
}
static char *command(const char *root){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);const char *exe=getenv("SAMOSA_QA_CONCURRENT_PROVIDER");if(!exe||!*exe)exe=self;fputc('[',f);json_quote(f,exe,strlen(exe));fputs(",\"--provider\",",f);json_quote(f,root,strlen(root));fputc(']',f);CHECK(!fclose(f));return out;}
static pid_t start(const AgentConfig *cfg,const char *req,const char *output){pid_t pid=fork();CHECK(pid>=0);if(!pid){execl(self,self,"--worker",cfg->state,cfg->owner,cfg->inference_command,req,output,(char*)NULL);_exit(2);}return pid;}
static void ready_all(const char *root,const pid_t *pids,unsigned n){double end=now()+20;for(;;){unsigned ready=0;for(unsigned i=0;i<n;i++){char p[512];path(p,root,"ready",i);if(!access(p,F_OK))ready++;int status;CHECK(!waitpid(pids[i],&status,WNOHANG));}if(ready==n)return;CHECK(now()<end);tick();}}
static char *wait_result(pid_t pid,const char *output){double end=now()+12;for(;;){int status;pid_t rc=waitpid(pid,&status,WNOHANG);CHECK(rc>=0);if(rc){CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));break;}CHECK(now()<end);tick();}FILE *f=fopen(output,"r");CHECK(f);char *out=malloc(65537);CHECK(out);size_t n=fread(out,1,65536,f);CHECK(n&&!ferror(f));out[n]=0;CHECK(!fclose(f));return out;}
static unsigned lease_count(const char *directory){DIR *d=opendir(directory);CHECK(d);struct dirent *e;unsigned n=0;while((e=readdir(d)))if(!strncmp(e->d_name,"run-",4))n++;CHECK(!closedir(d));return n;}
int main(int argc,char **argv){int guardian=command_guardian_dispatch(argc,argv);if(guardian>=0)return guardian;
    if(argc==3&&!strcmp(argv[1],"--provider"))return provider(argv[2]);
    if(argc==7&&!strcmp(argv[1],"--worker")){AgentConfig cfg={.state=argv[2],.owner=argv[3],.inference_command=argv[4]};double end=now()+20;char *out=NULL;int rc;
        for(;;){size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);rc=agent_dispatch(&cfg,argv[5],strlen(argv[5]),f);CHECK(!fclose(f));if(!rc)break;Json j;CHECK(json_parse(&j,out,n));char *error=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));int retry=error&&(!strcmp(error,"busy")||!strcmp(error,"store_busy"));free(error);if(!retry||now()>=end){fprintf(stderr,"worker refused: %s\n",out);break;}free(out);out=NULL;tick();}
        FILE *f=fopen(argv[6],"w");CHECK(f);CHECK(fwrite(out,1,strlen(out),f)==strlen(out));CHECK(!fclose(f));free(out);return rc;}
    char exe[4096];CHECK(realpath(argv[0],exe));self=exe;char root[]="/tmp/samosa-concurrent.XXXXXX";CHECK(mkdtemp(root));char directory[512];snprintf(directory,sizeof(directory),"%s/agent",root);char *cmd=command(root);AgentConfig cfg={.state=directory,.owner="alice",.inference_command=cmd};char *ids[9];
    for(unsigned i=0;i<9;i++){char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}",0);ids[i]=field(out,"conversation_id");free(out);}
    char req[1024],outputs[9][512];pid_t pids[9];for(unsigned i=0;i<2;i++){char op[32];snprintf(op,sizeof(op),"pair-%u",i);submit(req,ids[i],0,op,i);path(outputs[i],root,"pair-output",i);pids[i]=start(&cfg,req,outputs[i]);}ready_all(root,pids,2);
    char *out=run(&cfg,"cancel_run","pair-0",0);free(out);out=wait_result(pids[0],outputs[0]);expect_status(out,"cancelled");free(out);out=run(&cfg,"get_run","pair-1",0);expect_status(out,"running");Json j;CHECK(json_parse(&j,out,strlen(out)));int flag=json_get(&j,0,"cancel_requested");CHECK(flag>=0&&j.tokens[flag].type==JT_BOOL&&j.tokens[flag].end-j.tokens[flag].begin==5&&!memcmp(j.text+j.tokens[flag].begin,"false",5));free(out);release(root,1);out=wait_result(pids[1],outputs[1]);expect_status(out,"completed");free(out);state(&cfg,ids[0],1,1);state(&cfg,ids[1],2,2);
    size_t revisions[9]={1,2,0,0,0,0,0,0,0};char requests[9][1024];for(unsigned i=0;i<8;i++){reset(root,i);char op[32];snprintf(op,sizeof(op),"pool-%u",i);submit(requests[i],ids[i],revisions[i],op,i);path(outputs[i],root,"pool-output",i);pids[i]=start(&cfg,requests[i],outputs[i]);}ready_all(root,pids,8);
    submit(req,ids[2],1,"conflict",2);out=call(&cfg,req,1);expect_error(out,"busy");free(out);snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"compact\",\"conversation_id\":\"%s\",\"expected_revision\":1,\"operation_id\":\"compact-live\"}",ids[2]);out=call(&cfg,req,1);expect_error(out,"busy");free(out);state(&cfg,ids[2],1,1);
    submit(requests[8],ids[8],0,"pool-8",8);out=call(&cfg,requests[8],1);expect_error(out,"run_capacity");free(out);state(&cfg,ids[8],0,0);out=call(&cfg,requests[2],0);expect_status(out,"running");free(out);
    AgentConfig foreign=cfg;foreign.owner="other";out=run(&foreign,"cancel_run","pool-2",1);expect_error(out,"unknown_run");free(out);
    CHECK(!kill(pids[3],SIGKILL));int status;CHECK(waitpid(pids[3],&status,0)==pids[3]&&WIFSIGNALED(status));AgentConfig offline=cfg;offline.inference_command=NULL;out=call(&offline,requests[3],0);expect_status(out,"interrupted");char *again=call(&offline,requests[3],0);CHECK(!strcmp(out,again));free(out);free(again);
    for(unsigned i=0;i<8;i++)if(i!=3){char op[32];snprintf(op,sizeof(op),"pool-%u",i);out=run(&cfg,"get_run",op,0);expect_status(out,"running");free(out);}
    path(outputs[8],root,"pool-output",8);pids[8]=start(&cfg,requests[8],outputs[8]);double end=now()+10;char marker[512];path(marker,root,"ready",8);while(access(marker,F_OK)){CHECK(now()<end);tick();}
    for(unsigned i=0;i<9;i++)if(i!=3)release(root,i);
    for(unsigned i=0;i<9;i++)if(i!=3){out=wait_result(pids[i],outputs[i]);expect_status(out,"completed");again=call(&offline,requests[i],0);CHECK(!strcmp(out,again));free(out);free(again);state(&cfg,ids[i],revisions[i]+2,i==0?3:i==1?4:2);}
    state(&cfg,ids[3],1,1);
    unsigned before=lease_count(directory);char barrier[512];path(barrier,root,"precommit",8);CHECK(!setenv("SAMOSA_AGENT_TEST_LEASE_MARKER",barrier,1));submit(req,ids[8],2,"orphan-before-commit",8);pid_t dead=start(&cfg,req,outputs[8]);CHECK(!unsetenv("SAMOSA_AGENT_TEST_LEASE_MARKER"));end=now()+10;while(access(barrier,F_OK)){CHECK(now()<end);tick();}CHECK(lease_count(directory)==before+1);CHECK(!kill(dead,SIGKILL));CHECK(waitpid(dead,&status,0)==dead&&WIFSIGNALED(status));state(&cfg,ids[8],2,2);CHECK(lease_count(directory)==before);out=run(&cfg,"get_run","orphan-before-commit",1);expect_error(out,"unknown_run");free(out);
    for(unsigned i=0;i<9;i++)free(ids[i]);
    free(cmd);printf("PASS concurrent admission: %u checks; eight live workers, isolated cancel/death, capacity/revision/compact refusal, precommit lease recovery and offline retries; QA %s\n",checks,root);return 0;
}
