#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 8192
#include "find.h"
#include "command.h"
#include "evidence.h"
#include "json.h"
static int fail(FILE *out,const char *code){fputs("{\"contract_version\":1,\"error\":{\"code\":",out);json_quote(out,code,strlen(code));fputs("}}\n",out);return 1;}
static int identity(const char *s){if(!s||strlen(s)!=64)return 0;for(size_t i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static int envelope(Json *j,const char *value,size_t n){size_t version=0;return value&&json_parse(j,value,n)&&j->tokens[0].type==JT_OBJECT&&json_uint(j,json_get(j,0,"contract_version"),&version)&&version==1&&json_get(j,0,"error")<0;}
static void raw(FILE *out,const Json *j,int t){fwrite(j->text+j->tokens[t].begin,1,j->tokens[t].end-j->tokens[t].begin,out);}
static int literal_match(const char *value,const char *query){size_t n=strlen(query);for(const char *p=value;*p;p++){size_t i=0;while(i<n&&p[i]&&tolower((unsigned char)p[i])==tolower((unsigned char)query[i]))i++;if(i==n)return 1;}return 0;}
static int coverage_matches(const Json *a,int ac,const Json *b,int bc){const char *keys[]={"total_bytes","inspected_bytes"};for(size_t i=0;i<2;i++){size_t av=0,bv=0;if(!json_uint(a,json_get(a,ac,keys[i]),&av)||!json_uint(b,json_get(b,bc,keys[i]),&bv)||av!=bv)return 0;}char *as=json_string(a,json_get(a,ac,"status")),*bs=json_string(b,json_get(b,bc,"status"));int same=as&&bs&&!strcmp(as,bs);free(as);free(bs);return same;}
int agent_find(const AgentConfig *cfg,const char *input,size_t length,FILE *out){
    if(length>AGENT_REQUEST_MAX)return fail(out,"request_too_large");
    Json request;if(!envelope(&request,input,length))return fail(out,"invalid_request");
    const char *fields="|contract_version|op|job_id|query|max_inspect|";
    for(int t=1;t<request.tokens[0].next;){char *key=json_string(&request,t),field[128];int n=snprintf(field,sizeof(field),"|%s|",key?key:"");free(key);if(n<0||(size_t)n>=sizeof(field)||!strstr(fields,field))return fail(out,"unknown_field");t=request.tokens[request.tokens[t].next].next;}
    char *job=json_string(&request,json_get(&request,0,"job_id")),*query=json_string(&request,json_get(&request,0,"query"));size_t limit=0;
    const char *error=NULL;if(!identity(job)||!query||!*query||strlen(query)>128||!json_uint(&request,json_get(&request,0,"max_inspect"),&limit)||!limit||limit>3)error="invalid_request";
    if(!error&&(!cfg||!cfg->sources_command))error="dependency_unavailable";
    char *body=NULL,*response=NULL,*selected=NULL;size_t n=0,response_length=0,selected_length=0;FILE *stream=NULL;
    if(!error){stream=open_memstream(&body,&n);if(!stream)error="resource_limit";else {fprintf(stream,"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":",job);json_quote(stream,query,strlen(query));fputc('}',stream);if(fclose(stream))error="resource_limit";stream=NULL;}}
    if(!error)error=command_call(cfg->sources_command,body,n,10,&response,&response_length);
    Json candidates;int array=-1,index=-1;size_t returned=0,inspected=0,verified=0;char *candidate_coverage=NULL;
    if(!error){if(!envelope(&candidates,response,response_length))error="invalid_candidates";else {array=json_get(&candidates,0,"candidates");index=json_get(&candidates,0,"index");int exhaustive=json_get(&candidates,0,"exhaustive_current_filesystem");char *actual_job=json_string(&candidates,json_get(&candidates,index,"job_id"));candidate_coverage=json_string(&candidates,json_get(&candidates,0,"candidate_coverage"));if(array<0||candidates.tokens[array].type!=JT_ARRAY||candidates.tokens[array].count>10||!json_uint(&candidates,json_get(&candidates,0,"returned_candidates"),&returned)||returned!=(size_t)candidates.tokens[array].count||index<0||candidates.tokens[index].type!=JT_OBJECT||!actual_job||strcmp(actual_job,job)||exhaustive<0||candidates.tokens[exhaustive].type!=JT_BOOL||candidates.text[candidates.tokens[exhaustive].begin]!='f'||!candidate_coverage||(strcmp(candidate_coverage,"partial")&&strcmp(candidate_coverage,"complete_for_indexed_snapshot")))error="invalid_candidates";free(actual_job);}}
    if(!error){stream=open_memstream(&selected,&selected_length);if(!stream)error="resource_limit";}
    if(!error)for(int t=array+1;t<candidates.tokens[array].next&&inspected<limit;t=candidates.tokens[t].next,inspected++){
        char *source=json_string(&candidates,json_get(&candidates,t,"source_id")),*version=json_string(&candidates,json_get(&candidates,t,"version")),*expected=json_string(&candidates,json_get(&candidates,t,"evidence_id")),*locator=json_string(&candidates,json_get(&candidates,t,"locator")),*file_identity=json_string(&candidates,json_get(&candidates,t,"file_identity"));int range=json_get(&candidates,t,"range");size_t start=0,end=0;
        if(!identity(source)||!identity(version)||!identity(expected)||!identity(file_identity)||!locator||!*locator||strlen(locator)>4096||!json_uint(&candidates,json_get(&candidates,range,"start_byte"),&start)||!json_uint(&candidates,json_get(&candidates,range,"end_byte"),&end)||end<start||end-start>65536)error="invalid_candidates";
        char read[512],*evidence=NULL,*evidence_id=NULL,*literal=NULL;size_t evidence_length=0;Json ev;
        if(!error){int bytes=snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu}",source,version,start,end);error=command_call(cfg->sources_command,read,(size_t)bytes,10,&evidence,&evidence_length);}
        if(!error)error=agent_evidence_validate(evidence,evidence_length,source,version,start,end,0,&evidence_id);
        if(!error&&(!envelope(&ev,evidence,evidence_length)||strcmp(expected,evidence_id)||!coverage_matches(&candidates,json_get(&candidates,t,"coverage"),&ev,json_get(&ev,0,"coverage"))))error="invalid_evidence";
        if(!error){literal=json_string(&ev,json_get(&ev,0,"text"));if(!literal)error="invalid_evidence";else if(literal_match(literal,query)){if(verified++)fputc(',',stream);fputs("{\"locator\":",stream);json_quote(stream,locator,strlen(locator));fprintf(stream,",\"file_identity\":\"%s\",\"verification\":\"literal_match\",\"evidence\":",file_identity);raw(stream,&ev,0);fputc('}',stream);}}
        free(source);free(version);free(expected);free(locator);free(file_identity);free(evidence);free(evidence_id);free(literal);if(error)break;
    }
    if(stream){if(fclose(stream))error="resource_limit";stream=NULL;}
    if(!error&&selected_length>786432)error="response_too_large";
    if(!error){fprintf(out,"{\"contract_version\":1,\"policy\":\"candidate_inspect_verify_select_v1\",\"strategy\":\"explicit_literal_match\",\"exhaustive_current_filesystem\":false,\"index\":");raw(out,&candidates,index);fprintf(out,",\"candidate_count\":%zu,\"inspected_candidates\":%zu,\"uninspected_candidates\":%zu,\"verified_selections\":%zu,\"selection_coverage\":\"%s\",\"selected\":[",returned,inspected,returned-inspected,verified,inspected<returned||!strcmp(candidate_coverage,"partial")?"partial":"complete_for_returned_indexed_candidates");fwrite(selected,1,selected_length,out);fputs("]}\n",out);}
    free(job);free(query);free(body);free(response);free(selected);free(candidate_coverage);return error?fail(out,error):0;
}
