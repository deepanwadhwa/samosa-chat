#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "agent.h"
#include "command.h"
#include "json.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define JOB "1111111111111111111111111111111111111111111111111111111111111111"
#define SOURCE "2222222222222222222222222222222222222222222222222222222222222222"
#define EVIDENCE "3333333333333333333333333333333333333333333333333333333333333333"
#define FILE_ID "4444444444444444444444444444444444444444444444444444444444444444"
#define OTHER "5555555555555555555555555555555555555555555555555555555555555555"
static int helper(const char *mode,const char *log){char input[16385];size_t n=fread(input,1,sizeof(input)-1,stdin);input[n]=0;Json j;if(!json_parse(&j,input,n))return 1;char *op=json_string(&j,json_get(&j,0,"op"));if(!op)return 1;FILE *record=fopen(log,"a");if(!record){free(op);return 1;}fprintf(record,"%s\n",op);fclose(record);
    if(!strcmp(op,"index_search")){size_t count=!strcmp(mode,"bound")?10:1;fprintf(stdout,"{\"contract_version\":1,\"index\":{\"job_id\":\"%s\",\"content_coverage\":\"partial\",\"freshness\":\"captured_snapshot\"},\"exhaustive_current_filesystem\":false,\"candidate_coverage\":\"complete_for_indexed_snapshot\",\"returned_candidates\":%zu,\"candidates\":[",JOB,count);for(size_t i=0;i<count;i++){if(i)fputc(',',stdout);fprintf(stdout,"{\"locator\":\"file%zu.txt\",\"file_identity\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\",\"range\":{\"start_byte\":0,\"end_byte\":9},\"coverage\":{\"status\":\"complete\",\"total_bytes\":9,\"inspected_bytes\":%d},\"literal_preview\":\"FAKE PREVIEW 7319\"}",i,FILE_ID,SOURCE,SOURCE,EVIDENCE,!strcmp(mode,"coverage")?8:9);}fputs("]}",stdout);}
    else if(!strcmp(op,"read_text"))fprintf(stdout,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"range\":{\"start_byte\":0,\"end_byte\":9},\"coverage\":{\"status\":\"complete\",\"total_bytes\":9,\"inspected_bytes\":9},\"provenance\":{\"reader\":\"text-utf8-v1\"}}",SOURCE,!strcmp(mode,"version")?OTHER:SOURCE,!strcmp(mode,"identity")?OTHER:EVIDENCE,!strcmp(mode,"derived")?"derived":"literal",!strcmp(mode,"preview")?"No code!!":"Code 7319");else {free(op);return 1;}
    free(op);return 0;}
static char *run(const char *executable,const char *log,const char *mode,const char *query,size_t limit,int expected){char *command=NULL,*request=NULL,*out=NULL;size_t n,length;FILE *stream=open_memstream(&command,&n);CHECK(stream);fputc('[',stream);json_quote(stream,executable,strlen(executable));fputs(",\"--helper\",",stream);json_quote(stream,mode,strlen(mode));fputc(',',stream);json_quote(stream,log,strlen(log));fputc(']',stream);CHECK(!fclose(stream));stream=open_memstream(&request,&length);CHECK(stream);fprintf(stream,"{\"contract_version\":1,\"op\":\"find_files\",\"job_id\":\"%s\",\"query\":",JOB);json_quote(stream,query,strlen(query));fprintf(stream,",\"max_inspect\":%zu}",limit);CHECK(!fclose(stream));AgentConfig cfg={.sources_command=command,.inference_command="invalid-inference-must-never-be-called"};stream=open_memstream(&out,&n);CHECK(stream);CHECK(agent_dispatch(&cfg,request,length,stream)==expected);CHECK(!fclose(stream));Json result;CHECK(json_parse(&result,out,n));free(command);free(request);return out;}
static size_t number(const char *out,const char *key){Json j;size_t n;CHECK(json_parse(&j,out,strlen(out)));CHECK(json_uint(&j,json_get(&j,0,key),&n));return n;}
int main(int argc,char **argv){int guardian=command_guardian_dispatch(argc,argv);if(guardian>=0)return guardian;if(argc==4&&!strcmp(argv[1],"--helper"))return helper(argv[2],argv[3]);char fixture[]="/tmp/samosa-native-find-test-XXXXXX";CHECK(mkdtemp(fixture));char log[1024];snprintf(log,sizeof(log),"%s/log",fixture);
    char *out=run(argv[0],log,"valid","7319",3,0);CHECK(number(out,"verified_selections")==1);CHECK(strstr(out,"literal_match"));CHECK(strstr(out,"partial"));free(out);
    out=run(argv[0],log,"preview","7319",3,0);CHECK(number(out,"verified_selections")==0);CHECK(!strstr(out,"FAKE PREVIEW"));free(out);
    const char *bad[]={"version","identity","derived","coverage"};for(size_t i=0;i<4;i++){out=run(argv[0],log,bad[i],"7319",3,1);CHECK(strstr(out,"invalid_evidence"));CHECK(!strstr(out,"selected"));free(out);}
    CHECK(!unlink(log));out=run(argv[0],log,"bound","7319",3,0);CHECK(number(out,"verified_selections")==3);CHECK(number(out,"uninspected_candidates")==7);CHECK(strstr(out,"\"selection_coverage\":\"partial\""));free(out);FILE *record=fopen(log,"r");CHECK(record);char line[128];size_t reads=0,searches=0;while(fgets(line,sizeof(line),record)){if(!strcmp(line,"read_text\n"))reads++;if(!strcmp(line,"index_search\n"))searches++;}CHECK(!fclose(record));CHECK(reads==3&&searches==1);
    CHECK(!unlink(log));out=run(argv[0],log,"valid","7319",4,1);CHECK(strstr(out,"invalid_request"));CHECK(access(log,F_OK)!=0);free(out);
    out=run(argv[0],log,"valid","\"escaped\" \\ query",1,0);CHECK(number(out,"verified_selections")==0);free(out);
    char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("PASS native finding: %u checks; bounded inspection, forged preview/version/identity/coverage/derived refusals, no inference\n",checks);return 0;}
