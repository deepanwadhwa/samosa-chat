#include "agent.h"
#include "command.h"
#include <string.h>
int main(int argc,char **argv) {
    int guarded=command_guardian_dispatch(argc,argv);if(guarded>=0)return guarded;
    AgentConfig cfg={0};
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--sources-command")&&i+1<argc)cfg.sources_command=argv[++i];
        else if(!strcmp(argv[i],"--inference-command")&&i+1<argc)cfg.inference_command=argv[++i];
        else if(!strcmp(argv[i],"--state")&&i+1<argc)cfg.state=argv[++i];
        else if(!strcmp(argv[i],"--owner")&&i+1<argc)cfg.owner=argv[++i];
        else if(!strcmp(argv[i],"--action-root")&&i+1<argc)cfg.action_root=argv[++i];
        else if(!strcmp(argv[i],"--action-host-id")&&i+1<argc)cfg.action_host_id=argv[++i];
        else {fputs("usage: samosa-agent --sources-command JSON_ARGV --inference-command JSON_ARGV\n",stderr);return 2;}
    }
    char request[AGENT_REQUEST_MAX+1];size_t n=fread(request,1,sizeof(request),stdin);
    return ferror(stdin)?2:agent_dispatch(&cfg,request,n,stdout);
}
