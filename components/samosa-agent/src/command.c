#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "command.h"
#include "json.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;

#define RESPONSE_MAX 1048576u
static double now(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (double)ts.tv_sec+(double)ts.tv_nsec/1e9;
}
static int self_path(char path[4096]) {
#ifdef __APPLE__
    uint32_t size=4096;return _NSGetExecutablePath(path,&size)==0;
#else
    /* Keep the kernel's reference to this exact executable. Its resolved
     * pathname can be detached (and reported with a " (deleted)" suffix), or
     * replaced before spawn; neither pathname should select the guardian. */
    memcpy(path,"/proc/self/exe",sizeof("/proc/self/exe"));return 1;
#endif
}
int command_guardian_dispatch(int argc,char **argv) {
    if(argc<2||strcmp(argv[1],"--samosa-owned-helper"))return -1;
    if(argc<4)return 2;
    char *end=NULL;errno=0;double timeout=strtod(argv[2],&end);
    if(errno||!end||*end||timeout<=0||timeout>31||fcntl(3,F_GETFD)<0){
#ifdef SAMOSA_AGENT_TEST_FAULTS
        if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR"))fprintf(stderr,"QA guardian invalid control: errno=%d argc=%d\n",errno,argc);
#endif
        return 2;
    }
    if(fcntl(3,F_SETFD,FD_CLOEXEC)<0)return 2;
    posix_spawnattr_t attributes;posix_spawn_file_actions_t actions;
    int attr_ready=posix_spawnattr_init(&attributes)==0,actions_ready=posix_spawn_file_actions_init(&actions)==0;pid_t child=-1;
    int ok=attr_ready&&actions_ready;
    if(ok)ok=!posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP)&&!posix_spawnattr_setpgroup(&attributes,0)&&!posix_spawn_file_actions_addclose(&actions,3);
    int child_spawn_error=0;
    if(ok){child_spawn_error=posix_spawnp(&child,argv[3],&actions,&attributes,argv+3,environ);ok=child_spawn_error==0;}
    if(attr_ready)posix_spawnattr_destroy(&attributes);
    if(actions_ready)posix_spawn_file_actions_destroy(&actions);
    if(!ok){
#ifdef SAMOSA_AGENT_TEST_FAULTS
        if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR"))fprintf(stderr,"QA guardian spawn failure: code=%d errno=%d attributes=%d actions=%d\n",child_spawn_error,errno,attr_ready,actions_ready);
#endif
        close(3);return 2;
    }
    double deadline=now()+timeout;int completed=0;
    for(;;){siginfo_t info;memset(&info,0,sizeof(info));
        if(waitid(P_PID,(id_t)child,&info,WEXITED|WNOHANG|WNOWAIT)<0){if(errno!=EINTR)break;}
        else if(info.si_pid==child){completed=1;break;}
        if(now()>=deadline)break;
        struct pollfd watch={3,POLLIN|POLLHUP,0};int rc=poll(&watch,1,10);
        if(rc<0&&errno!=EINTR)break;
        if(rc>0&&(watch.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)))break;
    }
    /* Child remains unreaped so its process-group identity cannot be recycled. */
    (void)kill(-child,SIGKILL);int status=0;pid_t reaped;
    do{reaped=waitpid(child,&status,0);}while(reaped<0&&errno==EINTR);
    close(3);
#ifdef SAMOSA_AGENT_TEST_FAULTS
    if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR")&&(reaped!=child||!completed||!WIFEXITED(status)))
        fprintf(stderr,"QA guardian termination: completed=%d reaped=%d status=%d\n",completed,reaped==child,status);
#endif
    if(reaped!=child||!completed||!WIFEXITED(status))return 2;
#ifdef SAMOSA_AGENT_TEST_FAULTS
    if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR")&&WEXITSTATUS(status)>1)
        fprintf(stderr,"QA guardian target exit: status=%d\n",status);
#endif
    return WEXITSTATUS(status)<=1?WEXITSTATUS(status):2;
}

const char *command_call_controlled(const char *argv_json,const char *request,size_t length,double timeout,char **response,size_t *response_length,int (*cancelled)(void *),void *cancellation) {
    *response=NULL;*response_length=0;
    if(!argv_json || strlen(argv_json)>16384 || length>RESPONSE_MAX || timeout<=0 || timeout>30)return "invalid_configuration";
    if(cancelled&&cancelled(cancellation))return "cancelled";
    Json args;
    if(!json_parse(&args,argv_json,strlen(argv_json))||args.tokens[0].type!=JT_ARRAY||!args.tokens[0].count||args.tokens[0].count>32)return "invalid_configuration";
    char *argv[33]={0};int argc=0;const char *error=NULL;
    for(int t=1;t<args.tokens[0].next;t=args.tokens[t].next) {
        argv[argc]=json_string(&args,t);
        if(!argv[argc]||strlen(argv[argc])>4096){error="invalid_configuration";break;}argc++;
    }
    if(!error && !*argv[0])error="invalid_configuration";
    FILE *input=NULL;int pipefd[2]={-1,-1},lifeline[2]={-1,-1};pid_t pid=-1;int status=0,exited=0;
    char *bytes=NULL;size_t used=0;
    if(!error) {
        input=tmpfile();if(!input || fcntl(fileno(input),F_SETFD,FD_CLOEXEC)<0 || fwrite(request,1,length,input)!=length || fflush(input) || fseek(input,0,SEEK_SET))error="child_io";
    }
    if(!error && (pipe(pipefd)<0||pipe(lifeline)<0))error="child_io";
    if(!error) {
        fcntl(pipefd[0],F_SETFD,FD_CLOEXEC);fcntl(pipefd[1],F_SETFD,FD_CLOEXEC);fcntl(lifeline[0],F_SETFD,FD_CLOEXEC);fcntl(lifeline[1],F_SETFD,FD_CLOEXEC);
        bytes=malloc(RESPONSE_MAX+1);if(!bytes)error="resource_limit";
    }
    if(!error) {
        posix_spawn_file_actions_t actions;posix_spawnattr_t attributes;
        int actions_ready=posix_spawn_file_actions_init(&actions)==0;
        int attributes_ready=posix_spawnattr_init(&attributes)==0;
        if(!actions_ready||!attributes_ready)error="child_unavailable";
        char executable[4096],seconds[32];char *guard_argv[36]={0};
        if(!error&&!self_path(executable))error="child_unavailable";
        snprintf(seconds,sizeof(seconds),"%.6f",timeout+1);
        guard_argv[0]=executable;guard_argv[1]="--samosa-owned-helper";guard_argv[2]=seconds;
        for(int i=0;i<argc;i++)guard_argv[i+3]=argv[i];
        int diagnostics=0;
#ifdef SAMOSA_AGENT_TEST_FAULTS
        diagnostics=getenv("SAMOSA_AGENT_TEST_HELPER_STDERR")!=NULL;
#endif
        if(!error && (posix_spawn_file_actions_adddup2(&actions,fileno(input),STDIN_FILENO)||
            posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO)||
            (!diagnostics&&posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0))||
            posix_spawn_file_actions_adddup2(&actions,lifeline[0],3)||
            posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP)||
            posix_spawnattr_setpgroup(&attributes,0)))error="child_io";
        int inherited[]={pipefd[0],pipefd[1],lifeline[0],lifeline[1],fileno(input)};
        for(size_t i=0;!error&&i<sizeof(inherited)/sizeof(*inherited);i++)if(inherited[i]>3&&posix_spawn_file_actions_addclose(&actions,inherited[i]))error="child_io";
        if(!error){int spawned=posix_spawnp(&pid,executable,&actions,&attributes,guard_argv,environ);
            if(spawned){
#ifdef SAMOSA_AGENT_TEST_FAULTS
                if(diagnostics)fprintf(stderr,"QA guardian launch failure: code=%d executable=%s\n",spawned,executable);
#endif
                pid=-1;error="child_failed";
            }
        }
        if(actions_ready)posix_spawn_file_actions_destroy(&actions);
        if(attributes_ready)posix_spawnattr_destroy(&attributes);
        if(pid>0) {
            close(pipefd[1]);pipefd[1]=-1;close(lifeline[0]);lifeline[0]=-1;
            if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)<0)error="child_io";
        }
    }
    double deadline=now()+timeout;int eof=0;
    while(!error && pid>0 && (!eof || !exited)) {
        if(cancelled&&cancelled(cancellation)){error="cancelled";break;}
        if(now()>=deadline){error="deadline_exceeded";break;}
        if(!eof) {
            ssize_t got=read(pipefd[0],bytes+used,RESPONSE_MAX+1-used);
            if(got>0){used+=(size_t)got;if(used>RESPONSE_MAX)error="response_too_large";}
            else if(!got)eof=1;
            else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)error="child_io";
        }
        if(!exited) {
            siginfo_t info;memset(&info,0,sizeof(info));
            if(waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT)<0) {
#ifdef SAMOSA_AGENT_TEST_FAULTS
                if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR")&&errno!=EINTR)fprintf(stderr,"QA guardian waitid failure: errno=%d\n",errno);
#endif
                if(errno!=EINTR)error="child_failed";
            } else if(info.si_pid==pid)exited=1;
        }
        if(!error && (!eof||!exited)){struct pollfd watch={pipefd[0],POLLIN,0};(void)poll(&watch,eof?0:1,10);}
    }
    if(pid>0) {
        if(lifeline[1]>=0){close(lifeline[1]);lifeline[1]=-1;} /* Guardian independently stops/reaps its target group. */
        /* Keep the main child's PID reserved until its owned group is stopped. */
        pid_t got;do {got=waitpid(pid,&status,0);}while(got<0&&errno==EINTR);
        if(got!=pid && !error){
#ifdef SAMOSA_AGENT_TEST_FAULTS
            if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR"))fprintf(stderr,"QA guardian waitpid failure: errno=%d\n",errno);
#endif
            error="child_failed";
        }
        if(!error && (!WIFEXITED(status)||WEXITSTATUS(status)>1)){
#ifdef SAMOSA_AGENT_TEST_FAULTS
            if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR"))fprintf(stderr,"QA guardian failed exit: status=%d\n",status);
#endif
            error="child_failed";
        }
    }
    if(input)fclose(input);
    if(pipefd[0]>=0)close(pipefd[0]);
    if(pipefd[1]>=0)close(pipefd[1]);
    if(lifeline[0]>=0)close(lifeline[0]);
    if(lifeline[1]>=0)close(lifeline[1]);
    for(int i=0;i<33;i++)free(argv[i]);
    if(error){free(bytes);return error;}
    bytes[used]=0;*response=bytes;*response_length=used;return NULL;
}
const char *command_call(const char *argv_json,const char *request,size_t length,double timeout,char **response,size_t *response_length) {
    return command_call_controlled(argv_json,request,length,timeout,response,response_length,NULL,NULL);
}
