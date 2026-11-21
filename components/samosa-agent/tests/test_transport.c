#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "command.h"
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL native transport %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void round_trip(void){
    static const char request[]="{\"contract_version\":1,\"identity\":\"round-trip\"}";
    char *response=NULL;size_t length=0;
    const char *error=command_call("[\"/bin/cat\"]",request,sizeof(request)-1,10,&response,&length);
    if(error)fprintf(stderr,"Transport: %s\n",error);
    CHECK(!error);CHECK(length==sizeof(request)-1);CHECK(!memcmp(response,request,length));free(response);
}
#ifndef __APPLE__
static void detached_executable(const char *self){
    char root[]="/tmp/samosa-detached-helper.XXXXXX";CHECK(mkdtemp(root));
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/transport",root)>0);
    int input=open(self,O_RDONLY|O_CLOEXEC);CHECK(input>=0);
    int output=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0700);CHECK(output>=0);
    char bytes[16384];ssize_t n;
    while((n=read(input,bytes,sizeof(bytes)))>0){ssize_t offset=0;while(offset<n){ssize_t wrote=write(output,bytes+offset,(size_t)(n-offset));CHECK(wrote>0);offset+=wrote;}}
    CHECK(!n);CHECK(!close(input));CHECK(!fsync(output));CHECK(!close(output));
    char *args[]={path,"--unlinked",NULL};pid_t p;CHECK(!posix_spawn(&p,path,NULL,NULL,args,environ));
    int status;CHECK(waitpid(p,&status,0)==p);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));CHECK(access(path,F_OK)<0);CHECK(!rmdir(root));
}
#endif
int main(int argc,char **argv){int guarded=command_guardian_dispatch(argc,argv);if(guarded>=0)return guarded;
    if(argc==2&&!strcmp(argv[1],"--unlinked")){CHECK(!unlink(argv[0]));round_trip();return 0;}
    CHECK(argc==1);round_trip();
#ifndef __APPLE__
    char self[4096];CHECK(realpath(argv[0],self));detached_executable(self);
#endif
    printf("PASS native transport: %u checks; exact helper round trip and Linux detached executable reference\n",checks);return 0;
}
