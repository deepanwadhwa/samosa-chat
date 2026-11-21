#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "agent.h"
#include "command.h"
#include "json.h"
#include "sha256.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *call(const AgentConfig *cfg,const char *input){
    char *out=NULL;size_t length;FILE *stream=open_memstream(&out,&length);CHECK(stream);
    int rc=agent_dispatch(cfg,input,strlen(input),stream);CHECK(!fclose(stream));
    if(rc)fprintf(stderr,"response %s\n",out);
    CHECK(!rc);Json j;CHECK(json_parse(&j,out,length));return out;
}
static char *field(const char *out,const char *key){Json j;CHECK(json_parse(&j,out,strlen(out)));char *value=json_string(&j,json_get(&j,0,key));CHECK(value);return value;}
static char *operation(const char *op,const char *plan,const char *id){char *out=NULL;size_t length;FILE *stream=open_memstream(&out,&length);CHECK(stream);fprintf(stream,"{\"contract_version\":1,\"op\":\"%s\",\"plan_id\":\"%s\",\"plan_revision\":1,\"operation_id\":\"%s\"}",op,plan,id);CHECK(!fclose(stream));return out;}
static char *source_command(const char *exe,const char *marker){char *out=NULL;size_t length;FILE *stream=open_memstream(&out,&length);CHECK(stream);fputc('[',stream);json_quote(stream,exe,strlen(exe));fputs(",\"--source\",",stream);json_quote(stream,marker,strlen(marker));fputc(']',stream);CHECK(!fclose(stream));return out;}
int main(int argc,char **argv){
    int guardian=command_guardian_dispatch(argc,argv);if(guardian>=0)return guardian;
    if(argc==3&&!strcmp(argv[1],"--source")){
        int fd=open(argv[2],O_CREAT|O_WRONLY|O_EXCL,0600);if(fd<0)return 2;
        char pid[64];int n=snprintf(pid,sizeof(pid),"%ld",(long)getpid());if(write(fd,pid,(size_t)n)!=n){close(fd);return 2;}close(fd);
        for(;;)pause();
    }
    if(argc==6&&!strcmp(argv[1],"--worker")){
        char *cmd=source_command(argv[0],argv[4]);AgentConfig cfg={.state=argv[2],.action_root=argv[3],.sources_command=cmd};
        char *input=operation("plan_execute",argv[5],"interrupted-handoff");int rc=agent_dispatch(&cfg,input,strlen(input),stdout);free(input);free(cmd);return rc;
    }
    char fixture[]="/tmp/samosa-native-handoff-XXXXXX";CHECK(mkdtemp(fixture));
    char root[1024],state[1024],source[2048],destination[2048],marker[1024];
    snprintf(root,sizeof(root),"%s/root",fixture);snprintf(state,sizeof(state),"%s/state",fixture);snprintf(marker,sizeof(marker),"%s/source-started",fixture);
    CHECK(!mkdir(root,0700));snprintf(source,sizeof(source),"%s/source.txt",root);snprintf(destination,sizeof(destination),"%s/copy.txt",root);
    FILE *file=fopen(source,"w");CHECK(file);CHECK(fputs("code 7319",file)>=0);CHECK(!fclose(file));
    struct stat info;CHECK(!stat(source,&info));char metadata[256],identity[65],version[65];
#ifdef __APPLE__
    long nanos=info.st_mtimespec.tv_nsec;
#else
    long nanos=info.st_mtim.tv_nsec;
#endif
    int n=snprintf(metadata,sizeof(metadata),"file-identity-v1:%llu:%llu:%llu:%lld:%ld",(unsigned long long)info.st_dev,(unsigned long long)info.st_ino,(unsigned long long)info.st_size,(long long)info.st_mtime,nanos);
    hash_bytes(metadata,(size_t)n,identity);hash_bytes("code 7319",9,version);
    char input[2048];snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_create\",\"operation_id\":\"create\",\"host_id\":\"local-endpoint\",\"action\":\"copy\",\"files\":[{\"source\":\"source.txt\",\"destination\":\"copy.txt\",\"version\":\"%s\",\"file_identity\":\"%s\"}]}",version,identity);
    AgentConfig cfg={.state=state,.action_root=root};char *out=call(&cfg,input),*plan=field(out,"plan_id");free(out);
    char *request=operation("plan_approve",plan,"approve");out=call(&cfg,request);free(request);free(out);
    char *args[]={argv[0],"--worker",state,root,marker,plan,NULL};pid_t worker;CHECK(!posix_spawnp(&worker,argv[0],NULL,NULL,args,environ));
    int ready=0;for(int i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}CHECK(ready);
    CHECK(!stat(destination,&info));ino_t copied_inode=info.st_ino;
    CHECK(!kill(worker,SIGKILL));int status;CHECK(waitpid(worker,&status,0)==worker);CHECK(WIFSIGNALED(status));
    snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_get\",\"plan_id\":\"%s\"}",plan);out=call(&cfg,input);CHECK(strstr(out,"\"copied_items\":1"));free(out);
    request=operation("plan_execute",plan,"interrupted-handoff");out=call(&cfg,request);CHECK(strstr(out,"\"status\":\"interrupted\""));free(request);free(out);
    CHECK(!stat(destination,&info));CHECK(info.st_ino==copied_inode);
    cfg.sources_command="[\"/nonexistent-samosa-source\"]";request=operation("plan_execute",plan,"fresh-handoff");out=call(&cfg,request);
    CHECK(strstr(out,"\"effect_status\":\"completed\""));CHECK(strstr(out,"\"source_reconciliation\":{\"status\":\"failed\""));
    CHECK(!stat(destination,&info));CHECK(info.st_ino==copied_inode);
    cfg.action_root=NULL;cfg.sources_command=NULL;char *retried=call(&cfg,request);CHECK(!strcmp(out,retried));free(retried);free(request);free(out);free(plan);
    /* Guardian terminates its helper after parent death; reap via init on Linux. */
    file=fopen(marker,"r");CHECK(file);long helper=0;CHECK(fscanf(file,"%ld",&helper)==1);CHECK(!fclose(file));
    int gone=0;for(int i=0;i<500;i++){if(kill((pid_t)helper,0)&&errno==ESRCH){gone=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}CHECK(gone);
    char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    printf("PASS durable effect handoff: %u checks; SIGKILL during Source call, no effect replay, explicit failure and offline exact retry\n",checks);return 0;
}
