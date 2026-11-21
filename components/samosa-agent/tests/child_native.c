#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "agent.h"
#include "command.h"
#include "json.h"
#include <curl/curl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;static pid_t owned_worker=-1,owned_peer=-1;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL actual Agent child %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void cleanup(void){if(owned_worker>0){kill(owned_worker,SIGKILL);waitpid(owned_worker,NULL,0);}if(owned_peer>0){kill(owned_peer,SIGKILL);waitpid(owned_peer,NULL,0);}}
static void delay(void){struct timespec t={0,10000000};nanosleep(&t,NULL);}
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static char *call_maybe(const AgentConfig *cfg,const char *request,int optional){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);int rc=agent_dispatch(cfg,request,strlen(request),f);CHECK(!fclose(f));if(rc&&optional&&strstr(out,"unknown_run")){free(out);return NULL;}if(rc)fprintf(stderr,"Agent result: %s",out);CHECK(!rc);Json j;CHECK(json_parse(&j,out,n));return out;}
static char *call(const AgentConfig *cfg,const char *request){return call_maybe(cfg,request,0);}
static char *field(const char *body,const char *name){Json j;CHECK(json_parse(&j,body,strlen(body)));char *s=json_string(&j,json_get(&j,0,name));CHECK(s);return s;}
static char *args(const char **argv,size_t count){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);fputc('[',f);for(size_t i=0;i<count;i++){if(i)fputc(',',f);json_quote(f,argv[i],strlen(argv[i]));}fputc(']',f);CHECK(!fclose(f));return out;}
typedef struct {char bytes[16384];size_t used;} Response;
static size_t receive(char *bytes,size_t size,size_t count,void *opaque){Response *r=opaque;if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;if(n>sizeof(r->bytes)-r->used)return 0;memcpy(r->bytes+r->used,bytes,n);r->used+=n;return n;}
static int processing(const char *endpoint,const char *key){CURL *curl=curl_easy_init();CHECK(curl);char url[512],authorization[192];snprintf(url,sizeof(url),"%s/slots",endpoint);snprintf(authorization,sizeof(authorization),"Authorization: Bearer %s",key);
    struct curl_slist *headers=curl_slist_append(NULL,authorization);CHECK(headers);Response r={0};curl_easy_setopt(curl,CURLOPT_URL,url);curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(curl,CURLOPT_PROXY,"");curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,500L);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&r);CURLcode rc=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(curl);curl_slist_free_all(headers);
    if(rc==CURLE_OPERATION_TIMEDOUT||status==503){return 0;}
    CHECK(rc==CURLE_OK&&status==200);Json j;CHECK(json_parse(&j,r.bytes,r.used)&&j.tokens[0].type==JT_ARRAY&&j.tokens[0].count==1);int flag=json_get(&j,1,"is_processing");CHECK(flag>=0&&j.tokens[flag].type==JT_BOOL);return j.text[j.tokens[flag].begin]=='t';}
static char *public_operation(const char *command,const char *id){char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"%s\"}",id);char *out=NULL;size_t n;CHECK(!command_call(command,request,strlen(request),2,&out,&n));return out;}
static char *read_result(const char *path){FILE *f=fopen(path,"r");CHECK(f);char *s=malloc(65537);CHECK(s);size_t n=fread(s,1,65536,f);CHECK(n>0&&!ferror(f));s[n]=0;CHECK(!fclose(f));return s;}
static void wait_worker(pid_t *worker){double deadline=now()+6;for(;;){int status;pid_t got=waitpid(*worker,&status,WNOHANG);CHECK(got>=0);if(got==*worker){*worker=-1;CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);return;}CHECK(now()<deadline);delay();}}
int main(int argc,char **argv){int guarded=command_guardian_dispatch(argc,argv);if(guarded>=0)return guarded;
    if(argc==6&&!strcmp(argv[1],"--agent-worker")){AgentConfig cfg={.state=argv[2],.owner="qa",.inference_command=argv[3]};FILE *f=fopen(argv[5],"w");if(!f)return 2;int rc=agent_dispatch(&cfg,argv[4],strlen(argv[4]),f);if(fclose(f))rc=2;return rc;}
    if(argc!=7){fputs("usage: child-native INFERENCE_CLI ENDPOINT PROVIDER_ID ADMISSION_STATE PRIVATE_KEY_FILE QA_ROOT\n",stderr);return 2;}
    CHECK(!atexit(cleanup));char key[130]={0};FILE *key_file=fopen(argv[5],"r");CHECK(key_file);size_t key_length=fread(key,1,129,key_file);CHECK(key_length==64);CHECK(!fclose(key_file));
    const char *configuration[]={argv[1],"--llama-endpoint",argv[2],"--provider-id",argv[3],"--model-alias","agent-child-native","--admission-state",argv[4],"--owner","qa","--context-tokens","4096","--queue-capacity","4","--api-key-file",argv[5]};char *inference=args(configuration,sizeof(configuration)/sizeof(*configuration));
    char state[4096],output[4096],peer_output[4096],request[4096];CHECK(snprintf(state,sizeof(state),"%s/agent",argv[6])<(int)sizeof(state));CHECK(snprintf(output,sizeof(output),"%s/cancelled",argv[6])<(int)sizeof(output));CHECK(snprintf(peer_output,sizeof(peer_output),"%s/peer",argv[6])<(int)sizeof(peer_output));AgentConfig cfg={.state=state,.owner="qa",.inference_command=inference};
    char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"create\"}"),*conversation=field(out,"conversation_id");free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":0,\"operation_id\":\"warm\",\"question\":\"Compute 2 + 2. Answer only the integer.\"}",conversation);out=call(&cfg,request);CHECK(strstr(out,"\"status\":\"completed\"")&&strstr(out,"\"answer\":\"4\""));free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":2,\"operation_id\":\"long\",\"question\":\"Write every integer from 1 to 10000, separated by spaces. Do not abbreviate or skip any integers.\"}",conversation);
    char self[4096];CHECK(realpath(argv[0],self));owned_worker=fork();CHECK(owned_worker>=0);if(!owned_worker){execl(self,self,"--agent-worker",state,inference,request,output,(char*)NULL);_exit(2);}
    double deadline=now()+10;int observed=0;char *run=NULL,*child=NULL;
    while(now()<deadline){int status;CHECK(waitpid(owned_worker,&status,WNOHANG)==0);out=call_maybe(&cfg,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"long\"}",1);if(!out){delay();continue;}Json j;CHECK(json_parse(&j,out,strlen(out)));int entries=json_get(&j,0,"children");
        if(entries>=0)for(int t=entries+1;t<j.tokens[entries].next;t=j.tokens[t].next){char *kind=json_string(&j,json_get(&j,t,"kind")),*status_text=json_string(&j,json_get(&j,t,"status"));if(kind&&status_text&&!strcmp(kind,"generate")&&!strcmp(status_text,"running")){free(child);child=json_string(&j,json_get(&j,t,"operation_id"));}free(kind);free(status_text);}
        if(child&&processing(argv[2],key)){run=field(out,"run_id");observed=1;free(out);break;}free(out);delay();}
    CHECK(observed&&child&&run);
    /* An unrelated tracked worker queues on the same owner/provider while the
     * target is observed active. Cancelling the target must not cancel this ID. */
    char peer_request[512];snprintf(peer_request,sizeof(peer_request),"{\"contract_version\":1,\"op\":\"generate\",\"operation_id\":\"unrelated-native\",\"provider_id\":\"%s\",\"prompt\":\"Compute 2 + 2. Answer only the integer.\",\"max_output_tokens\":32}",argv[3]);
    owned_peer=fork();CHECK(owned_peer>=0);if(!owned_peer){char *reply=NULL;size_t n;const char *error=command_call(inference,peer_request,strlen(peer_request),10,&reply,&n);FILE *f=fopen(peer_output,"w");if(!f)_exit(2);if(!error)fwrite(reply,1,n,f);int rc=error?2:0;if(fclose(f))rc=2;free(reply);_exit(rc);}
    deadline=now()+1;int queued=0;while(now()<deadline){char *reply=public_operation(inference,"unrelated-native");if(strstr(reply,"\"status\":\"running\"")){queued=1;free(reply);break;}free(reply);delay();}CHECK(queued);
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"cancel_run\",\"operation_id\":\"long\"}");CHECK(strstr(out,"\"cancel_requested\":true"));free(out);wait_worker(&owned_worker);
    out=read_result(output);CHECK(strstr(out,"\"status\":\"cancelled\""));CHECK(strstr(out,"observed_cancelled"));CHECK(strstr(out,child));char *saved=strdup(out);CHECK(saved);free(out);
    out=public_operation(inference,child);CHECK(strstr(out,"\"status\":\"cancelled\""));CHECK(strstr(out,run));free(out);wait_worker(&owned_peer);
    out=read_result(peer_output);CHECK(strstr(out,"\"answer\":\"4\""));free(out);CHECK(!processing(argv[2],key));
    AgentConfig offline=cfg;offline.inference_command=NULL;out=call(&offline,request);CHECK(!strcmp(saved,out));free(out);free(saved);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":3,\"operation_id\":\"survivor\",\"question\":\"Compute 2 + 2. Answer only the integer.\"}",conversation);out=call(&cfg,request);CHECK(strstr(out,"\"status\":\"completed\"")&&strstr(out,"\"answer\":\"4\""));free(out);
    free(run);run=NULL;free(child);child=NULL;
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":5,\"operation_id\":\"deadline\",\"question\":\"Write every integer from 1 to 10000, separated by spaces. Do not abbreviate or skip any integers.\",\"timeout_ms\":2500}",conversation);
    owned_worker=fork();CHECK(owned_worker>=0);if(!owned_worker){execl(self,self,"--agent-worker",state,inference,request,output,(char*)NULL);_exit(2);}
    deadline=now()+2;observed=0;
    while(now()<deadline){int status;CHECK(waitpid(owned_worker,&status,WNOHANG)==0);out=call_maybe(&cfg,"{\"contract_version\":1,\"op\":\"get_run\",\"operation_id\":\"deadline\"}",1);if(!out){delay();continue;}Json j;CHECK(json_parse(&j,out,strlen(out)));int entries=json_get(&j,0,"children");
        if(entries>=0)for(int t=entries+1;t<j.tokens[entries].next;t=j.tokens[t].next){char *kind=json_string(&j,json_get(&j,t,"kind")),*state_text=json_string(&j,json_get(&j,t,"status"));if(kind&&state_text&&!strcmp(kind,"generate")&&!strcmp(state_text,"running")){free(child);child=json_string(&j,json_get(&j,t,"operation_id"));}free(kind);free(state_text);}
        if(child&&processing(argv[2],key)){run=field(out,"run_id");observed=1;free(out);break;}free(out);delay();}
    CHECK(observed&&child&&run);wait_worker(&owned_worker);
    out=read_result(output);CHECK(strstr(out,"\"status\":\"failed\""));CHECK(strstr(out,"deadline_exceeded"));CHECK(strstr(out,"\"cancel_requested\":false"));CHECK(!strstr(out,"unconfirmed"));saved=strdup(out);CHECK(saved);free(out);
    out=public_operation(inference,child);CHECK(strstr(out,"\"status\":\"failed\"")||strstr(out,"\"status\":\"cancelled\""));CHECK(strstr(out,run));free(out);
    out=call(&offline,request);CHECK(!strcmp(saved,out));free(out);free(saved);CHECK(!processing(argv[2],key));
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"submit_chat\",\"conversation_id\":\"%s\",\"expected_revision\":6,\"operation_id\":\"deadline-survivor\",\"question\":\"Compute 2 + 2. Answer only the integer.\"}",conversation);out=call(&cfg,request);CHECK(strstr(out,"\"status\":\"completed\"")&&strstr(out,"\"answer\":\"4\""));free(out);
    free(run);free(child);free(conversation);free(inference);memset(key,0,sizeof(key));printf("PASS actual Agent child control: %u checks; observed active engine, durable parent/child cancellation, unrelated queued answer, offline retry and survivor\n",checks);return 0;
}
