#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "agent.h"
#include "command.h"
#include "child.h"
#include "json.h"
#include <sqlite3.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;static const char *executable;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL child operations %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void delay(long ns){struct timespec t={0,ns};nanosleep(&t,NULL);}
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static void bind(sqlite3_stmt *s,int n,const char *v){CHECK(sqlite3_bind_text(s,n,v,-1,SQLITE_TRANSIENT)==SQLITE_OK);}
static sqlite3_stmt *prepare(sqlite3 *db,const char *sql){sqlite3_stmt *s=NULL;CHECK(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK);return s;}
static void finish(sqlite3_stmt *s){CHECK(sqlite3_step(s)==SQLITE_DONE);CHECK(sqlite3_finalize(s)==SQLITE_OK);}
static void reference(const char *id,const char *parent){fputs(",\"operation_id\":",stdout);json_quote(stdout,id,strlen(id));fputs(",\"parent_operation_id\":",stdout);json_quote(stdout,parent,strlen(parent));}
static int fixture_provider(const char *mode,const char *state,const char *marker){
    char input[65537];size_t n=fread(input,1,sizeof(input),stdin);Json j;CHECK(json_parse(&j,input,n));char *op=json_string(&j,json_get(&j,0,"op"));CHECK(op);
    if(!strcmp(op,"describe")){fputs("{\"contract_version\":1,\"provider_id\":\"child-fixture\",\"context_tokens\":8192,\"capabilities\":[\"text\",\"count_tokens\",\"operations\",\"cancellation\",\"operation_parents\"]}",stdout);free(op);return 0;}
    sqlite3 *db=NULL;CHECK(sqlite3_open(state,&db)==SQLITE_OK);CHECK(sqlite3_busy_timeout(db,1000)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS calls(id TEXT PRIMARY KEY,parent TEXT NOT NULL,status TEXT NOT NULL,cancel INTEGER NOT NULL DEFAULT 0)",NULL,NULL,NULL)==SQLITE_OK);
    char *id=json_string(&j,json_get(&j,0,"operation_id"));CHECK(id);
    if(!strcmp(op,"operation_cancel")||!strcmp(op,"operation_get")) {
        if(!strcmp(mode,"deny")){fputs("{\"contract_version\":1,\"error\":{\"code\":\"control_unavailable\"}}",stdout);free(op);free(id);CHECK(sqlite3_close(db)==SQLITE_OK);return 1;}
        if(!strcmp(op,"operation_cancel")){sqlite3_stmt *s=prepare(db,!strcmp(mode,"race")?"UPDATE calls SET cancel=1,status='completed' WHERE id=? AND status='running'":"UPDATE calls SET cancel=1 WHERE id=? AND status='running'");bind(s,1,id);finish(s);}
        if(!strcmp(mode,"delayed")&&!strcmp(op,"operation_cancel")) {
            /* Observe the worker's committed terminal row, without requiring a
             * third guardian startup inside the production control budget. */
            for(unsigned attempt=0;attempt<25;attempt++) {
                sqlite3_stmt *probe=prepare(db,"SELECT status,cancel FROM calls WHERE id=?");bind(probe,1,id);int result=sqlite3_step(probe);
                CHECK(result==SQLITE_ROW||result==SQLITE_DONE);
                int pending=result==SQLITE_ROW&&sqlite3_column_int(probe,1)&&!strcmp((const char*)sqlite3_column_text(probe,0),"running");
                CHECK(sqlite3_finalize(probe)==SQLITE_OK);
                if(!pending)break;
                delay(10000000);
            }
        }
        sqlite3_stmt *s=prepare(db,"SELECT parent,status,cancel FROM calls WHERE id=?");bind(s,1,id);int rc=sqlite3_step(s);
        if(rc!=SQLITE_ROW){CHECK(rc==SQLITE_DONE);fputs("{\"contract_version\":1,\"error\":{\"code\":\"unknown_operation\"}}",stdout);CHECK(sqlite3_finalize(s)==SQLITE_OK);free(op);free(id);CHECK(sqlite3_close(db)==SQLITE_OK);return 1;}
        const char *parent=(const char*)sqlite3_column_text(s,0),*status=(const char*)sqlite3_column_text(s,1);
        fputs("{\"contract_version\":1",stdout);reference(id,parent);fputs(",\"status\":",stdout);json_quote(stdout,status,strlen(status));fprintf(stdout,",\"cancel_requested\":%s}",sqlite3_column_int(s,2)?"true":"false");
        CHECK(sqlite3_finalize(s)==SQLITE_OK);free(op);free(id);CHECK(sqlite3_close(db)==SQLITE_OK);return 0;
    }
    char *parent=json_string(&j,json_get(&j,0,"parent_operation_id")),*prompt=json_string(&j,json_get(&j,0,"prompt"));CHECK(parent&&prompt);
    int generation=!strcmp(op,"generate");
    if(generation&&strcmp(mode,"normal")&&strcmp(mode,"forged")){int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,id,strlen(id))==(ssize_t)strlen(id));CHECK(!fsync(fd));CHECK(!close(fd));if(!strcmp(mode,"delayed"))delay(250000000);}
    sqlite3_stmt *s=prepare(db,"INSERT INTO calls(id,parent,status) VALUES(?,?,?)");bind(s,1,id);bind(s,2,parent);bind(s,3,generation?"running":"completed");finish(s);
    int stopped=0;
    if(generation&&strcmp(mode,"normal")&&strcmp(mode,"forged")) {
        double deadline=now()+15;
        while(now()<deadline){s=prepare(db,"SELECT cancel,status FROM calls WHERE id=?");bind(s,1,id);CHECK(sqlite3_step(s)==SQLITE_ROW);stopped=sqlite3_column_int(s,0);int completed=!strcmp((const char*)sqlite3_column_text(s,1),"completed");CHECK(sqlite3_finalize(s)==SQLITE_OK);if(stopped){if(completed)stopped=0;break;}delay(10000000);}
    }
    s=prepare(db,"UPDATE calls SET status=? WHERE id=?");bind(s,1,stopped?"cancelled":"completed");bind(s,2,id);finish(s);
    fputs("{\"contract_version\":1",stdout);reference(id,!strcmp(mode,"forged")&&generation?"forged-parent":parent);
    if(stopped)fputs(",\"error\":{\"code\":\"cancelled\"}",stdout);
    else if(!generation)fprintf(stdout,",\"provider_id\":\"child-fixture\",\"input_tokens\":%zu,\"context_tokens\":8192",strlen(prompt));
    else fprintf(stdout,",\"provider_id\":\"child-fixture\",\"answer\":\"42\",\"usage\":{\"input_tokens\":%zu,\"output_tokens\":2}",strlen(prompt));
    fputs("}",stdout);CHECK(sqlite3_close(db)==SQLITE_OK);free(op);free(id);free(parent);free(prompt);return stopped?1:0;
}
static char *command(const char *self,const char *mode,const char *state,const char *marker){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);const char *fixture=getenv("SAMOSA_QA_CHILD_PROVIDER");if(!fixture||!*fixture)fixture=self;const char *args[]={fixture,"--fixture",mode,state,marker};fputc('[',f);for(unsigned i=0;i<5;i++){if(i)fputc(',',f);json_quote(f,args[i],strlen(args[i]));}fputc(']',f);CHECK(!fclose(f));return out;}
static char *call(const AgentConfig *cfg,const char *input,int expected){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);int rc=agent_dispatch(cfg,input,strlen(input),f);CHECK(!fclose(f));if(rc!=expected)fprintf(stderr,"result: %s",out);CHECK(rc==expected);Json j;CHECK(json_parse(&j,out,n));return out;}
static char *field(const char *input,const char *key){Json j;CHECK(json_parse(&j,input,strlen(input)));char *s=json_string(&j,json_get(&j,0,key));CHECK(s);return s;}
static void submit(char out[2048],const char *conversation,size_t revision,const char *id){snprintf(out,2048,"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":%zu,\"operation_id\":\"%s\",\"question\":\"Compute 6 * 7\"}",conversation,revision,id);}
static pid_t start(const AgentConfig *cfg,const char *input,const char *path){pid_t p=fork();CHECK(p>=0);if(!p){execl(executable,executable,"--agent-worker",cfg->state,cfg->owner,cfg->inference_command,input,path,(char*)NULL);_exit(2);}return p;}
/* Sanitizer process startup is separate from the six-second cancellation check. */
static void ready(pid_t p,const char *path){double deadline=now()+20;struct stat st;while(stat(path,&st)||!st.st_size){int status;CHECK(waitpid(p,&status,WNOHANG)==0);if(now()>=deadline)fprintf(stderr,"Child startup marker timed out: %s\n",path);CHECK(now()<deadline);delay(10000000);}}
static void wait_worker(pid_t p){double deadline=now()+6;for(;;){int status;pid_t got=waitpid(p,&status,WNOHANG);CHECK(got>=0);if(got==p){CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));return;}CHECK(now()<deadline);delay(10000000);}}
static char *read_file(const char *path){FILE *f=fopen(path,"r");CHECK(f);char *s=malloc(65537);CHECK(s);size_t n=fread(s,1,65536,f);CHECK(!ferror(f)&&n);s[n]=0;CHECK(!fclose(f));return s;}
static void scenario(const char *self,const char *root,const char *mode,const char *outcome,const char *child_status){
    char state[512],provider[512],marker[512],output[512],req[2048];snprintf(state,sizeof(state),"%s/agent-%s",root,mode);snprintf(provider,sizeof(provider),"%s/provider-%s.sqlite3",root,mode);snprintf(marker,sizeof(marker),"%s/ready-%s",root,mode);snprintf(output,sizeof(output),"%s/result-%s",root,mode);
    char *cmd=command(self,mode,provider,marker);AgentConfig cfg={.state=state,.owner="alice",.inference_command=cmd};char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}",0),*conversation=field(out,"conversation_id");free(out);
    AgentConfig peer={0};pid_t peer_worker=-1;char *peer_cmd=NULL,*peer_conversation=NULL;char peer_state[512],peer_marker[512],peer_output[512];
    if(!strcmp(mode,"wait")) {
        snprintf(peer_state,sizeof(peer_state),"%s/peer-agent",root);snprintf(peer_marker,sizeof(peer_marker),"%s/peer-ready",root);snprintf(peer_output,sizeof(peer_output),"%s/peer-result",root);
        peer_cmd=command(self,"wait",provider,peer_marker);peer=(AgentConfig){.state=peer_state,.owner="alice",.inference_command=peer_cmd};
        out=call(&peer,"{\"contract_version\":1,\"op\":\"create\"}",0);peer_conversation=field(out,"conversation_id");free(out);submit(req,peer_conversation,0,"blocked");peer_worker=start(&peer,req,peer_output);ready(peer_worker,peer_marker);
    }
    submit(req,conversation,0,"blocked");pid_t p=start(&cfg,req,output);ready(p,marker);
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"blocked\"}",0);CHECK(strstr(out,"\"children\":["));char *run=field(out,"run_id");free(out);
    AgentConfig foreign=cfg;foreign.owner="other";out=call(&foreign,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"blocked\"}",1);CHECK(strstr(out,"unknown_run"));free(out);
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"blocked\"}",0);CHECK(strstr(out,"\"cancel_requested\":true"));free(out);wait_worker(p);
    out=read_file(output);CHECK(strstr(out,"\"status\":\"cancelled\""));if(!strstr(out,outcome))fprintf(stderr,"Unexpected child outcome for %s: %s\n",mode,out);CHECK(strstr(out,outcome));Json j;CHECK(json_parse(&j,out,strlen(out)));int children=json_get(&j,0,"children");CHECK(children>=0&&j.tokens[children].count==2);int second=j.tokens[children+1].next;
    char *status=json_string(&j,json_get(&j,second,"status")),*child=json_string(&j,json_get(&j,second,"operation_id"));CHECK(status&&child&&!strcmp(status,child_status));free(status);
    AgentConfig offline=cfg;offline.inference_command=NULL;submit(req,conversation,0,"blocked");char *retry=call(&offline,req,0);CHECK(!strcmp(out,retry));free(retry);free(out);
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"%s\"}",child);char *reply=NULL;size_t length;const char *error=command_call(cmd,request,strlen(request),5,&reply,&length);if(error)fprintf(stderr,"Public child status transport %s: %s\n",mode,error);CHECK(!error);
    if(strcmp(mode,"deny")){CHECK(strstr(reply,run));CHECK(strstr(reply,child_status));}else CHECK(strstr(reply,"control_unavailable"));free(reply);free(child);free(run);free(cmd);free(conversation);
    if(peer_worker>0) {
        int process_status;CHECK(waitpid(peer_worker,&process_status,WNOHANG)==0);out=call(&peer,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"blocked\"}",0);CHECK(strstr(out,"\"cancel_requested\":false"));CHECK(strstr(out,"\"status\":\"running\""));
        CHECK(json_parse(&j,out,strlen(out)));int entries=json_get(&j,0,"children"),generation=j.tokens[entries+1].next;char *peer_id=json_string(&j,json_get(&j,generation,"operation_id"));CHECK(peer_id);free(out);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"%s\"}",peer_id);reply=NULL;CHECK(!command_call(peer_cmd,request,strlen(request),5,&reply,&length));CHECK(strstr(reply,"\"status\":\"running\""));CHECK(strstr(reply,"\"cancel_requested\":false"));free(reply);free(peer_id);
        out=call(&peer,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"blocked\"}",0);free(out);wait_worker(peer_worker);free(peer_cmd);free(peer_conversation);
    }
}
int main(int argc,char **argv){int guardian=command_guardian_dispatch(argc,argv);if(guardian>=0)return guardian;
    if(argc==7&&!strcmp(argv[1],"--agent-worker")){AgentConfig cfg={.state=argv[2],.owner=argv[3],.inference_command=argv[4]};FILE *f=fopen(argv[6],"w");if(!f)return 2;int rc=agent_dispatch(&cfg,argv[5],strlen(argv[5]),f);if(fclose(f))rc=2;return rc;}
    if(argc==5&&!strcmp(argv[1],"--fixture"))return fixture_provider(argv[2],argv[3],argv[4]);
    char self[4096];CHECK(realpath(argv[0],self));executable=self;char root[]="/tmp/samosa-child-operations.XXXXXX";CHECK(mkdtemp(root));
    scenario(self,root,"wait","observed_cancelled","cancelled");scenario(self,root,"delayed","observed_cancelled","cancelled");scenario(self,root,"race","observed_completed","completed");scenario(self,root,"deny","unconfirmed","unconfirmed");
    char state[512],provider[512],marker[512],req[2048];snprintf(state,sizeof(state),"%s/forged-agent",root);snprintf(provider,sizeof(provider),"%s/forged-provider",root);snprintf(marker,sizeof(marker),"%s/forged-marker",root);
    char *cmd=command(self,"forged",provider,marker);AgentConfig cfg={.state=state,.owner="alice",.inference_command=cmd};char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}",0),*conversation=field(out,"conversation_id");free(out);
    submit(req,conversation,0,"forged");out=call(&cfg,req,0);CHECK(strstr(out,"invalid_child_response"));CHECK(strstr(out,"\"status\":\"failed\""));free(out);free(cmd);free(conversation);
    char id[80];cfg.parent_operation_id="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";CHECK(!agent_child_id(&cfg,1,id));CHECK(agent_child_id(&cfg,17,id));CHECK(agent_child_id(&cfg,0,id));
    printf("PASS child operations: %u checks; durable parent, public cancel/status, startup/completion race, forged-parent and unconfirmed control; QA %s\n",checks,root);return 0;
}
