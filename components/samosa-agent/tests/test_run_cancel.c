#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "agent.h"
#include "command.h"
#include "json.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;static const char *executable;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL run cancellation %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static void pause_short(void){struct timespec t={0,10000000};nanosleep(&t,NULL);}
static char *call(const AgentConfig *cfg,const char *request,int expected){char *result=NULL;size_t n;FILE *out=open_memstream(&result,&n);CHECK(out);
    int rc=agent_dispatch(cfg,request,strlen(request),out);CHECK(!fclose(out));if(rc!=expected)fprintf(stderr,"Unexpected run result: %s",result);CHECK(rc==expected);Json j;CHECK(json_parse(&j,result,n));return result;}
static char *field(const char *input,const char *name){Json j;CHECK(json_parse(&j,input,strlen(input)));char *s=json_string(&j,json_get(&j,0,name));CHECK(s);return s;}
static char *command(const char *self,const char *mode,const char *marker){char *s=NULL;size_t n;FILE *f=open_memstream(&s,&n);CHECK(f);
    fputc('[',f);json_quote(f,self,strlen(self));fputs(",\"--provider\",",f);json_quote(f,mode,strlen(mode));fputc(',',f);json_quote(f,marker,strlen(marker));fputc(']',f);CHECK(!fclose(f));return s;}
static int provider(const char *mode,const char *marker){char request[65537];size_t n=fread(request,1,sizeof(request),stdin);Json j;if(!json_parse(&j,request,n))return 2;
    char *op=json_string(&j,json_get(&j,0,"op"));if(!op)return 2;
    if(!strcmp(op,"describe"))fputs("{\"contract_version\":1,\"provider_id\":\"cancel-fixture\",\"context_tokens\":8192,\"capabilities\":[\"text\",\"count_tokens\",\"request_budgets\"]}",stdout);
    else {char *prompt=json_string(&j,json_get(&j,0,"prompt"));if(!prompt){free(op);return 2;}
        if(!strcmp(op,"count_tokens"))fprintf(stdout,"{\"contract_version\":1,\"provider_id\":\"cancel-fixture\",\"input_tokens\":%zu,\"context_tokens\":8192}",strlen(prompt));
        else if(!strcmp(op,"generate")) {
            if(!strcmp(mode,"hang")) {pid_t child=fork();if(!child){for(;;)pause();}if(child<0)return 2;
                int fd=open(marker,O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0)return 2;char ids[128];int len=snprintf(ids,sizeof(ids),"%d %d %d",(int)getpid(),(int)child,(int)getppid());
                if(write(fd,ids,(size_t)len)!=len||fsync(fd)||close(fd))return 2;
                for(;;)pause();
            }
            fprintf(stdout,"{\"contract_version\":1,\"provider_id\":\"cancel-fixture\",\"answer\":\"42\",\"usage\":{\"input_tokens\":%zu,\"output_tokens\":2}}",strlen(prompt));
        }else {free(prompt);free(op);return 2;}free(prompt);
    }free(op);return 0;}
static void request(char *out,size_t cap,const char *conversation,size_t revision,const char *operation){int n=snprintf(out,cap,"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":%zu,\"operation_id\":\"%s\",\"question\":\"Compute 6 * 7\"}",conversation,revision,operation);CHECK(n>0&&(size_t)n<cap);}
static pid_t worker(const AgentConfig *cfg,const char *request,const char *output,const char *barrier){pid_t child=fork();CHECK(child>=0);if(!child){execl(executable,executable,"--worker",cfg->state,cfg->owner,cfg->inference_command,request,output,barrier?barrier:"",(char*)NULL);_exit(2);}return child;}
static void await_file(const char *path,pid_t child){double deadline=now()+5;struct stat st;while(stat(path,&st)||!st.st_size){int status;pid_t got=waitpid(child,&status,WNOHANG);if(got!=0)fprintf(stderr,"Worker exited before marker %s: pid=%d wait=%d status=%d errno=%d\n",path,(int)child,(int)got,status,errno);CHECK(got==0);CHECK(now()<deadline);pause_short();}}
static void await_worker(pid_t child,int code){double deadline=now()+5;int status;for(;;){pid_t got=waitpid(child,&status,WNOHANG);CHECK(got>=0);if(got==child){CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==code);break;}CHECK(now()<deadline);pause_short();}}
static char *read_result(const char *path){FILE *f=fopen(path,"r");CHECK(f);char *out=malloc(65537);CHECK(out);size_t n=fread(out,1,65536,f);CHECK(!ferror(f)&&n>0);out[n]=0;CHECK(!fclose(f));return out;}
static void helper_gone(const char *path){FILE *f=fopen(path,"r");CHECK(f);int ids[3];CHECK(fscanf(f,"%d %d %d",&ids[0],&ids[1],&ids[2])==3);CHECK(!fclose(f));
    double deadline=now()+5;for(unsigned i=0;i<3;i++){CHECK(ids[i]>1);while(!kill((pid_t)ids[i],0)){CHECK(now()<deadline);pause_short();}CHECK(errno==ESRCH);}}
static void expect_error(const AgentConfig *cfg,const char *request,const char *code){char *s=call(cfg,request,1);CHECK(strstr(s,code));free(s);}
int main(int argc,char **argv){int guarded=command_guardian_dispatch(argc,argv);if(guarded>=0)return guarded;
    if(argc==8&&!strcmp(argv[1],"--worker")){AgentConfig cfg={.state=argv[2],.owner=argv[3],.inference_command=argv[4]};if(*argv[7])setenv("SAMOSA_AGENT_TEST_RESERVED_MARKER",argv[7],1);FILE *f=fopen(argv[6],"w");if(!f)return 2;int rc=agent_dispatch(&cfg,argv[5],strlen(argv[5]),f);if(fclose(f))rc=2;return rc;}
    if(argc==4&&!strcmp(argv[1],"--provider"))return provider(argv[2],argv[3]);
    char self[4096];CHECK(realpath(argv[0],self));executable=self;char root[]="/tmp/samosa-run-cancel.XXXXXX";CHECK(mkdtemp(root));
    char state[512],peer_state[512],marker[512],peer_marker[512],output[512],peer_output[512],barrier[512],dead_output[512];
    snprintf(state,sizeof(state),"%s/state",root);snprintf(peer_state,sizeof(peer_state),"%s/peer",root);snprintf(marker,sizeof(marker),"%s/helper",root);snprintf(peer_marker,sizeof(peer_marker),"%s/peer-helper",root);
    snprintf(output,sizeof(output),"%s/result",root);snprintf(peer_output,sizeof(peer_output),"%s/peer-result",root);snprintf(barrier,sizeof(barrier),"%s/barrier",root);snprintf(dead_output,sizeof(dead_output),"%s/dead-result",root);
    char *normal=command(self,"normal",marker),*hanging=command(self,"hang",marker),*peer_hanging=command(self,"hang",peer_marker);
    AgentConfig cfg={.state=state,.owner="alice",.inference_command=normal};char req[2048];
    char *created=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}",0),*conversation=field(created,"conversation_id");free(created);
    request(req,sizeof(req),conversation,0,"first");char *first=call(&cfg,req,0);if(!strstr(first,"\"status\":\"completed\""))fprintf(stderr,"Unexpected initial run: %s\n",first);CHECK(strstr(first,"\"status\":\"completed\""));
    char *reply=call(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"first\"}",0);CHECK(!strcmp(first,reply));free(reply);free(first);
    AgentConfig peer={.state=peer_state,.owner="alice",.inference_command=peer_hanging};created=call(&peer,"{\"contract_version\":1,\"op\":\"create\"}",0);char *peer_conversation=field(created,"conversation_id");free(created);
    request(req,sizeof(req),peer_conversation,0,"blocked");pid_t peer_child=worker(&peer,req,peer_output,NULL);await_file(peer_marker,peer_child);
    cfg.inference_command=hanging;request(req,sizeof(req),conversation,2,"blocked");pid_t child=worker(&cfg,req,output,NULL);await_file(marker,child);
    reply=call(&cfg,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"blocked\"}",0);CHECK(strstr(reply,"\"status\":\"running\""));char *run_id=field(reply,"run_id");free(reply);
    AgentConfig foreign=cfg;foreign.owner="mallory";expect_error(&foreign,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"blocked\"}","unknown_run");
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"missing\"}","unknown_run");
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\"}","invalid_request");
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"blocked\",\"run_id\":\"%s\"}",run_id);expect_error(&cfg,req,"invalid_request");
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"cancel_run\",\"run_id\":\"%s\"}",run_id);reply=call(&cfg,req,0);CHECK(strstr(reply,"\"cancel_requested\":true"));free(reply);free(run_id);
    await_worker(child,0);helper_gone(marker);reply=read_result(output);CHECK(strstr(reply,"\"status\":\"cancelled\""));CHECK(strstr(reply,"\"code\":\"cancelled\""));CHECK(strstr(reply,"\"cancel_requested\":true"));
    char *saved=call(&cfg,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"blocked\"}",0);CHECK(!strcmp(reply,saved));free(saved);
    cfg.inference_command=NULL;request(req,sizeof(req),conversation,2,"blocked");saved=call(&cfg,req,0);CHECK(!strcmp(reply,saved));free(saved);free(reply);
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"get\",\"conversation_id\":\"%s\"}",conversation);reply=call(&cfg,req,0);CHECK(strstr(reply,"\"revision\":3"));Json j;CHECK(json_parse(&j,reply,strlen(reply)));int messages=json_get(&j,0,"messages");CHECK(messages>=0&&j.tokens[messages].count==3);CHECK(strstr(reply,"\"selected_scope\":{\"kind\":\"chat\"}"));free(reply);
    int wait_status;CHECK(waitpid(peer_child,&wait_status,WNOHANG)==0);reply=call(&peer,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"blocked\"}",0);CHECK(strstr(reply,"\"status\":\"running\""));CHECK(strstr(reply,"\"cancel_requested\":false"));free(reply);
    reply=call(&peer,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"blocked\"}",0);free(reply);await_worker(peer_child,0);helper_gone(peer_marker);
    cfg.inference_command=normal;request(req,sizeof(req),conversation,3,"survivor");reply=call(&cfg,req,0);CHECK(strstr(reply,"\"status\":\"completed\""));CHECK(strstr(reply,"\"revision\":5"));free(reply);
    request(req,sizeof(req),conversation,5,"dead-cancel");child=worker(&cfg,req,dead_output,barrier);await_file(barrier,child);
    reply=call(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"dead-cancel\"}",0);CHECK(strstr(reply,"\"cancel_requested\":true"));free(reply);
    CHECK(!kill(child,SIGKILL));CHECK(waitpid(child,&wait_status,0)==child&&WIFSIGNALED(wait_status));
    reply=call(&cfg,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"dead-cancel\"}",0);CHECK(strstr(reply,"\"status\":\"cancelled\""));free(reply);
    cfg.inference_command=NULL;reply=call(&cfg,req,0);CHECK(strstr(reply,"\"status\":\"cancelled\""));free(reply);
    /* An expired host budget refuses admission; finished timed retries stay offline. */
    cfg.inference_command=normal;created=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}",0);char *timed=field(created,"conversation_id");free(created);
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":0,\"operation_id\":\"invalid-budget\",\"question\":\"Hi\",\"timeout_ms\":0}",timed);expect_error(&cfg,req,"invalid_request");
    request(req,sizeof(req),timed,0,"expired");cfg.deadline_monotonic=now()-1;expect_error(&cfg,req,"deadline_exceeded");cfg.deadline_monotonic=0;
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"get\",\"conversation_id\":\"%s\"}",timed);reply=call(&cfg,req,0);CHECK(strstr(reply,"\"revision\":0"));free(reply);
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":0,\"operation_id\":\"timed-ok\",\"question\":\"Hi\",\"timeout_ms\":10000}",timed);reply=call(&cfg,req,0);CHECK(strstr(reply,"\"status\":\"completed\""));
    cfg.inference_command=NULL;cfg.deadline_monotonic=now()-1;saved=call(&cfg,req,0);CHECK(!strcmp(reply,saved));free(saved);free(reply);cfg.deadline_monotonic=0;
    char *budget=strstr(req,"10000}");CHECK(budget);memcpy(budget,"10001}",6);expect_error(&cfg,req,"idempotency_conflict");
    CHECK(!unlink(marker));cfg.inference_command=hanging;
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":2,\"operation_id\":\"timed-hang\",\"question\":\"Hi\",\"timeout_ms\":10000}",timed);
    double started=now();reply=call(&cfg,req,0);CHECK(now()-started<15);CHECK(strstr(reply,"\"status\":\"failed\""));CHECK(strstr(reply,"deadline_exceeded"));CHECK(strstr(reply,"\"cancel_requested\":false"));helper_gone(marker);
    cfg.inference_command=NULL;saved=call(&cfg,req,0);CHECK(!strcmp(reply,saved));free(saved);free(reply);
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"get\",\"conversation_id\":\"%s\"}",timed);reply=call(&cfg,req,0);CHECK(strstr(reply,"\"revision\":3"));CHECK(json_parse(&j,reply,strlen(reply)));messages=json_get(&j,0,"messages");CHECK(messages>=0&&j.tokens[messages].count==3);free(reply);free(timed);
    free(normal);free(hanging);free(peer_hanging);free(conversation);free(peer_conversation);
    printf("PASS durable run cancellation: %u checks; peer isolation, owner/refusal, interrupted helper, last-good, exact retry and death recovery; QA %s\n",checks,root);return 0;
}
