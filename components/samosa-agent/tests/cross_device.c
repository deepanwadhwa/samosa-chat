#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "command.h"
#include "json.h"
#include "sha256.h"
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *call(const char *command,const char *request){char *out=NULL;size_t n;CHECK(!command_call(command,request,strlen(request),10,&out,&n));Json j;CHECK(json_parse(&j,out,n));CHECK(json_get(&j,0,"error")<0);return out;}
static void cleanup(char *root){char *args[]={"find",root,"-depth","-delete",NULL};pid_t pid;int status;CHECK(!posix_spawnp(&pid,"find",NULL,NULL,args,environ));CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
int main(int argc,char **argv){int guardian=command_guardian_dispatch(argc,argv);if(guardian>=0)return guardian;CHECK(argc==2);char root[]="/tmp/samosa-native-cross-root-XXXXXX",state[]="/dev/shm/samosa-native-cross-state-XXXXXX";CHECK(mkdtemp(root));CHECK(mkdtemp(state));struct stat a,b;CHECK(!stat(root,&a)&&!stat(state,&b));CHECK(a.st_dev!=b.st_dev);char path[512];snprintf(path,sizeof(path),"%s/source.txt",root);FILE *file=fopen(path,"w");CHECK(file);CHECK(fputs("7319",file)>=0);CHECK(!fclose(file));CHECK(!stat(path,&a));
#ifdef __APPLE__
long nanos=a.st_mtimespec.tv_nsec;
#else
long nanos=a.st_mtim.tv_nsec;
#endif
char metadata[256],identity[65],version[65];int count=snprintf(metadata,sizeof(metadata),"file-identity-v1:%llu:%llu:%llu:%lld:%ld",(unsigned long long)a.st_dev,(unsigned long long)a.st_ino,(unsigned long long)a.st_size,(long long)a.st_mtime,nanos);hash_bytes(metadata,(size_t)count,identity);hash_bytes("7319",4,version);char *command=NULL;size_t n;FILE *stream=open_memstream(&command,&n);CHECK(stream);fputc('[',stream);json_quote(stream,argv[1],strlen(argv[1]));fprintf(stream,",\"--state\",\"%s\",\"--action-root\",\"%s\"]",state,root);CHECK(!fclose(stream));char input[1024];snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_create\",\"operation_id\":\"create\",\"action\":\"copy\",\"host_id\":\"local-endpoint\",\"files\":[{\"source\":\"source.txt\",\"destination\":\"copy.txt\",\"version\":\"%s\",\"file_identity\":\"%s\"}]}",version,identity);char *out=call(command,input);Json j;CHECK(json_parse(&j,out,strlen(out)));char *plan=json_string(&j,json_get(&j,0,"plan_id"));CHECK(plan);free(out);snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_approve\",\"plan_id\":\"%s\",\"plan_revision\":1,\"operation_id\":\"approve\"}",plan);out=call(command,input);free(out);snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_execute\",\"plan_id\":\"%s\",\"plan_revision\":1,\"operation_id\":\"execute\"}",plan);out=call(command,input);CHECK(strstr(out,"cross_device_anchor"));CHECK(strstr(out,"\"copied_items\":0"));free(out);snprintf(path,sizeof(path),"%s/copy.txt",root);CHECK(access(path,F_OK)!=0);snprintf(path,sizeof(path),"%s/source.txt",root);CHECK(!stat(path,&b)&&a.st_ino==b.st_ino&&b.st_size==4);free(plan);snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_create\",\"operation_id\":\"create-move\",\"action\":\"move\",\"host_id\":\"local-endpoint\",\"files\":[{\"source\":\"source.txt\",\"destination\":\"move.txt\",\"version\":\"%s\",\"file_identity\":\"%s\"}]}",version,identity);out=call(command,input);CHECK(json_parse(&j,out,strlen(out)));plan=json_string(&j,json_get(&j,0,"plan_id"));CHECK(plan);free(out);snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_approve\",\"plan_id\":\"%s\",\"plan_revision\":1,\"operation_id\":\"approve-move\"}",plan);out=call(command,input);free(out);snprintf(input,sizeof(input),"{\"contract_version\":1,\"op\":\"plan_execute\",\"plan_id\":\"%s\",\"plan_revision\":1,\"operation_id\":\"execute-move\"}",plan);out=call(command,input);CHECK(strstr(out,"cross_device_move"));CHECK(strstr(out,"\"moved_items\":0"));free(out);snprintf(path,sizeof(path),"%s/move.txt",root);CHECK(access(path,F_OK)!=0);snprintf(path,sizeof(path),"%s/source.txt",root);CHECK(!stat(path,&b)&&a.st_ino==b.st_ino&&b.st_size==4);

free(plan);free(command);cleanup(root);cleanup(state);puts("PASS native cross-device identity anchor refusal; no publication/original unchanged");return 0;}
