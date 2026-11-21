#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "agent.h"
#include "command.h"
#include "store.h"
#include "context.h"
#include "evidence.h"
#include "find.h"
#include "actions.h"
#include "child.h"
#include "budget.h"
#include "source_call.h"
#include "json.h"
static int fail(FILE *out,const char *code) {
    fputs("{\"contract_version\":1,\"error\":{\"code\":",out);json_quote(out,code,strlen(code));fputs("}}\n",out);return 1;
}
static int id_valid(const char *s) {
    if(!s||strlen(s)!=64)return 0;
    for(int i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int envelope(Json *j,const char *text,size_t n) {
    size_t version;
    return json_parse(j,text,n)&&j->tokens[0].type==JT_OBJECT&&json_uint(j,json_get(j,0,"contract_version"),&version)&&version==1;
}
static char *error_code(const Json *j){
    char *code=json_string(j,json_get(j,json_get(j,0,"error"),"code"));
    if(!code||!*code||strlen(code)>128){free(code);return NULL;}
    for(const char *p=code;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p)){free(code);return NULL;}
    return code;
}
static char *inference_request(const char *op,const char *provider,const char *prompt,const char *session,size_t revision,const char *child,const char *parent,size_t budget,size_t *length) {
    char *body=NULL;FILE *stream=open_memstream(&body,length);if(!stream)return NULL;
    fputs("{\"contract_version\":1,\"op\":",stream);json_quote(stream,op,strlen(op));
    fputs(",\"provider_id\":",stream);json_quote(stream,provider,strlen(provider));
    fputs(",\"prompt\":",stream);json_quote(stream,prompt,strlen(prompt));
    if(!strcmp(op,"generate"))fputs(",\"max_output_tokens\":256",stream);
    if(session){fputs(",\"session_id\":",stream);json_quote(stream,session,strlen(session));fprintf(stream,",\"expected_session_revision\":%zu",revision);}
    if(child){fputs(",\"operation_id\":",stream);json_quote(stream,child,strlen(child));fputs(",\"parent_operation_id\":",stream);json_quote(stream,parent,strlen(parent));}
    if(budget)fprintf(stream,",\"timeout_ms\":%zu",budget);
    fputs("}",stream);if(fclose(stream)){free(body);return NULL;}return body;
}
static const char *new_session(const AgentConfig *cfg,const char *provider,char **session) {
    char *request=NULL;size_t n=0;FILE *stream=open_memstream(&request,&n);if(!stream)return "resource_limit";
    fputs("{\"contract_version\":1,\"op\":\"session_create\",\"provider_id\":",stream);json_quote(stream,provider,strlen(provider));fputc('}',stream);
    if(fclose(stream)){free(request);return "resource_limit";}
    char *response=NULL;size_t length=0;const char *error=agent_call(cfg,cfg->inference_command,request,n,&response,&length);Json j;free(request);
    if(!error) {
        if(!envelope(&j,response,length)||json_get(&j,0,"error")>=0)error="session_unavailable";
        else {
            char *pin=json_string(&j,json_get(&j,0,"provider_id"));size_t revision;
            *session=json_string(&j,json_get(&j,0,"session_id"));
            if(!pin||strcmp(pin,provider)||!*session||strlen(*session)!=48||!json_uint(&j,json_get(&j,0,"revision"),&revision)||revision!=0)error="invalid_session";
            free(pin);
        }
    }
    free(response);return error;
}
int agent_replay_session(const AgentConfig *cfg,const char *input,size_t length,const char *admitted,const char *history,const char *continuation,FILE *out) {
    if(length>AGENT_REQUEST_MAX)return fail(out,"request_too_large");
    Json request;size_t version;
    if(!envelope(&request,input,length))return fail(out,"invalid_request");
    char *op=json_string(&request,json_get(&request,0,"op"));if(!op)return fail(out,"invalid_request");
    if(!strcmp(op,"undo_plan_create")||!strcmp(op,"plan_create")||!strcmp(op,"plan_get")||!strcmp(op,"plan_approve")||!strcmp(op,"plan_execute")){free(op);return agent_actions(cfg,input,length,out);}
    int describe=!strcmp(op,"describe"),question_op=!strcmp(op,"question"),chat_op=!strcmp(op,"chat"),find_op=!strcmp(op,"find_files");free(op);
    if(find_op)return agent_find(cfg,input,length,out);
    if(!describe && !question_op&&!chat_op)return store_dispatch(cfg,input,length,out);
    const char *fields=describe?"|op|contract_version|":chat_op?"|op|contract_version|question|":"|op|contract_version|question|source_id|version|start_byte|end_byte|start_page|end_page|";
    for(int t=1;t<request.tokens[0].next;) {
        char *key=json_string(&request,t),match[128];int n=snprintf(match,sizeof(match),"|%s|",key?key:"");free(key);
        if(n<0||(size_t)n>=sizeof(match)||!strstr(fields,match))return fail(out,"unknown_field");
        t=request.tokens[request.tokens[t].next].next;
    }
    if(describe){fputs("{\"contract_version\":1,\"component\":\"samosa-agent\",\"version\":\"0.1.0\",\"operations\":[\"undo_plan_create\",\"plan_create\",\"plan_get\",\"plan_approve\",\"plan_execute\",\"chat\",\"submit_chat\",\"find_files\",\"question\",\"create\",\"get\",\"submit\",\"get_run\",\"cancel_run\",\"events\",\"compact\"]}\n",out);return 0;}
    const char *error=NULL;size_t start=0,end=0;
    int pdf=json_get(&request,0,"start_page")>=0||json_get(&request,0,"end_page")>=0;
    int bytes=json_get(&request,0,"start_byte")>=0||json_get(&request,0,"end_byte")>=0;
    const char *start_field=pdf?"start_page":"start_byte",*end_field=pdf?"end_page":"end_byte";
    char *source=json_string(&request,json_get(&request,0,"source_id"));
    char *source_version=json_string(&request,json_get(&request,0,"version"));
    char *question=json_string(&request,json_get(&request,0,"question"));
    char *evidence=NULL,*descriptor=NULL,*count_result=NULL,*generated=NULL,*provider=NULL,*prompt=NULL,*body=NULL,*answer=NULL;
    char *evidence_id=NULL,*session=NULL,*sliced=NULL,*provider_error=NULL;size_t evidence_length=0,descriptor_length=0,count_length=0,generated_length=0,body_length=0,session_revision=0;
    size_t history_count=0,omitted=0;int session_capable=0,session_replayed=0,operation_capable=0,budget_capable=0;unsigned child_index=0;
    char child_id[80]={0};
    if(!question||!*question||strlen(question)>2048)error=chat_op?"invalid_request":"invalid_selector";
    else if(!chat_op&&(!id_valid(source)||!id_valid(source_version)||
       (pdf&&bytes)||(!pdf&&!bytes)||!json_uint(&request,json_get(&request,0,start_field),&start)||!json_uint(&request,json_get(&request,0,end_field),&end)||end<start||
       (pdf?(start<1||end-start>=4):(end-start>65536))))error="invalid_selector";
    else if(!cfg||(!chat_op&&!admitted&&!cfg->sources_command)||!cfg->inference_command)error="dependency_unavailable";
    char reader_request[512];
    if(!error&&!chat_op) {
        int n=snprintf(reader_request,sizeof(reader_request),"{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"%s\":%zu,\"%s\":%zu}",pdf?"read_pdf":"read_text",source,source_version,start_field,start,end_field,end);
        if(admitted){evidence=strdup(admitted);if(!evidence)error="resource_limit";else evidence_length=strlen(evidence);}
        else error=agent_source_call(cfg,&child_index,pdf?"source_read_pdf":"source_read_text",reader_request,(size_t)n,&evidence,&evidence_length);
    }
    Json ev={0},desc={0},count={0},gen={0};
    if(!error&&!chat_op&&envelope(&ev,evidence,evidence_length)&&json_get(&ev,0,"error")>=0){provider_error=error_code(&ev);error=provider_error?provider_error:"source_failed";}
    if(!error&&!chat_op){error=agent_evidence_validate(evidence,evidence_length,source,source_version,start,end,pdf,&evidence_id);if(!error&&!envelope(&ev,evidence,evidence_length))error="invalid_evidence";}
    if(!error&&!chat_op) {
        /* Canonical root bytes stabilize initial and restored evidence prefixes. */
        evidence_length=ev.tokens[0].end-ev.tokens[0].begin;
        memmove(evidence,evidence+ev.tokens[0].begin,evidence_length);evidence[evidence_length]=0;
    }
    static const char describe_request[]="{\"contract_version\":1,\"op\":\"describe\"}";
    size_t context=0;
    if(!error)error=agent_call(cfg,cfg->inference_command,describe_request,sizeof(describe_request)-1,&descriptor,&descriptor_length);
    if(!error) {
        if(!envelope(&desc,descriptor,descriptor_length)||json_get(&desc,0,"error")>=0)error="provider_unavailable";
        else {
            provider=json_string(&desc,json_get(&desc,0,"provider_id"));
            int capabilities=json_get(&desc,0,"capabilities"),text=0,token=0,operations=0,cancellation=0,parents=0;
            if(capabilities>=0&&desc.tokens[capabilities].type==JT_ARRAY) {
                for(int t=capabilities+1;t<desc.tokens[capabilities].next;t=desc.tokens[t].next) {
                    char *cap=json_string(&desc,t);if(cap){if(!strcmp(cap,"text"))text=1;if(!strcmp(cap,"count_tokens"))token=1;if(!strcmp(cap,"sessions"))session_capable=1;if(!strcmp(cap,"operations"))operations=1;if(!strcmp(cap,"cancellation"))cancellation=1;if(!strcmp(cap,"operation_parents"))parents=1;if(!strcmp(cap,"request_budgets"))budget_capable=1;}free(cap);
                }
            }
            if(!provider||!*provider||strlen(provider)>128||!text||!token||!json_uint(&desc,json_get(&desc,0,"context_tokens"),&context)||context<=256)error="unsupported_capability";
            operation_capable=cfg->parent_operation_id&&operations&&cancellation&&parents;
            if(cfg->deadline_monotonic&&!budget_capable)error="unsupported_capability";
        }
    }
    if(!error&&continuation) {
        Json ref;size_t previous_context=0;char *pin=NULL;
        if(!json_parse(&ref,continuation,strlen(continuation)))error="invalid_session";
        else {
            pin=json_string(&ref,json_get(&ref,0,"provider_id"));
            if(!pin||strcmp(pin,provider)||!json_uint(&ref,json_get(&ref,0,"context_tokens"),&previous_context)||previous_context!=context)error="provider_unavailable";
            else if(session_capable&&json_get(&ref,0,"session_id")>=0) {
                session=json_string(&ref,json_get(&ref,0,"session_id"));
                if(!session||strlen(session)!=48||!json_uint(&ref,json_get(&ref,0,"revision"),&session_revision))error="invalid_session";
            }
        }
        free(pin);
    }
    if(!error)error=context_history(history,&history_count);
    size_t input_tokens=0;
    for(int pass=0;!error&&pass<10;pass++) {
        free(sliced);sliced=context_slice(history,omitted);if(!sliced){error="resource_limit";break;}
        free(prompt);prompt=context_prompt(evidence,question,history?sliced:NULL);if(!prompt){error="resource_limit";break;}
        if(operation_capable){error=agent_child_id(cfg,++child_index,child_id);if(error)break;}
        free(body);body=inference_request("count_tokens",provider,prompt,NULL,0,operation_capable?child_id:NULL,cfg->parent_operation_id,cfg->deadline_monotonic?agent_remaining_ms(cfg):0,&body_length);if(!body){error="resource_limit";break;}
        free(count_result);count_result=NULL;error=operation_capable?agent_child_call(cfg,child_id,"count_tokens",body,body_length,&count_result,&count_length):agent_call(cfg,cfg->inference_command,body,body_length,&count_result,&count_length);
        if(error)break;
        if(!envelope(&count,count_result,count_length)){error="token_count_failed";break;}
        if(json_get(&count,0,"error")>=0){provider_error=error_code(&count);error=provider_error?provider_error:"token_count_failed";break;}
        char *pin=json_string(&count,json_get(&count,0,"provider_id"));
        if(!pin||strcmp(pin,provider)||!json_uint(&count,json_get(&count,0,"context_tokens"),&version)||version!=context||!json_uint(&count,json_get(&count,0,"input_tokens"),&input_tokens))error="provider_mismatch";
        free(pin);if(error||input_tokens<=context-256)break;
        if(omitted==history_count){error="context_limit";break;}
        size_t remaining=history_count-omitted;omitted+=(remaining+1)/2;
        if(pass==9)error="context_limit";
    }
    if(!error&&session_capable&&!session)error=new_session(cfg,provider,&session);
    for(int attempt=0;!error&&attempt<2;attempt++) {
        if(operation_capable){error=agent_child_id(cfg,++child_index,child_id);if(error)break;}
        free(body);body=inference_request("generate",provider,prompt,session,session_revision,operation_capable?child_id:NULL,cfg->parent_operation_id,cfg->deadline_monotonic?agent_remaining_ms(cfg):0,&body_length);if(!body){error="resource_limit";break;}
        free(generated);generated=NULL;error=operation_capable?agent_child_call(cfg,child_id,"generate",body,body_length,&generated,&generated_length):agent_call(cfg,cfg->inference_command,body,body_length,&generated,&generated_length);
        if(!error&&envelope(&gen,generated,generated_length)&&json_get(&gen,0,"error")>=0&&session&&attempt==0) {
            char *code=json_string(&gen,json_get(&gen,json_get(&gen,0,"error"),"code"));
            int replay=code&&(!strcmp(code,"unknown_session")||!strcmp(code,"stale_session"));free(code);
            if(replay){free(session);session=NULL;session_revision=0;session_replayed=1;error=new_session(cfg,provider,&session);continue;}
        }
        break;
    }
    size_t generated_input=0,output_tokens=0;
    if(!error) {
        if(!envelope(&gen,generated,generated_length))error="generation_failed";
        else if(json_get(&gen,0,"error")>=0){provider_error=error_code(&gen);error=provider_error?provider_error:"generation_failed";}
        else {
            char *pin=json_string(&gen,json_get(&gen,0,"provider_id"));int usage=json_get(&gen,0,"usage");answer=json_string(&gen,json_get(&gen,0,"answer"));
            if(!pin||strcmp(pin,provider)||!answer||strlen(answer)>65536||!json_uint(&gen,json_get(&gen,usage,"input_tokens"),&generated_input)||generated_input!=input_tokens||!json_uint(&gen,json_get(&gen,usage,"output_tokens"),&output_tokens)||output_tokens>256)error="provider_mismatch";
            free(pin);
            if(!error&&session) {
                char *returned=json_string(&gen,json_get(&gen,0,"session_id"));size_t revision;
                if(!returned||strcmp(returned,session)||!json_uint(&gen,json_get(&gen,0,"session_revision"),&revision)||revision!=session_revision+1)error="invalid_session";
                else session_revision=revision;
                free(returned);
            }
        }
    }
    if(!error) {
        fputs("{\"contract_version\":1,\"answer\":",out);json_quote(out,answer,strlen(answer));
        if(chat_op)fputs(",\"citations\":[],\"admitted_evidence\":[",out);
        else fprintf(out,",\"citations\":[{\"evidence_id\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"range\":{\"%s\":%zu,\"%s\":%zu}}],\"admitted_evidence\":[",evidence_id,source,source_version,start_field,start,end_field,end);
        if(evidence)fwrite(evidence,1,evidence_length,out);
        fputs("],\"model_request\":{\"provider_id\":",out);json_quote(out,provider,strlen(provider));
        fputs(",\"prompt\":",out);json_quote(out,prompt,strlen(prompt));
        fprintf(out,",\"input_tokens\":%zu,\"reserved_output_tokens\":256},\"context_policy\":{\"history_messages\":%zu,\"omitted_history_messages\":%zu,\"kept_history_messages\":%zu,\"session_replayed\":%s},\"continuation\":{\"provider_id\":",input_tokens,history_count,omitted,history_count-omitted,session_replayed?"true":"false");json_quote(out,provider,strlen(provider));fprintf(out,",\"context_tokens\":%zu",context);
        if(session){fputs(",\"session_id\":",out);json_quote(out,session,strlen(session));fprintf(out,",\"revision\":%zu",session_revision);}fputs("},\"usage\":",out);
        int usage=json_get(&gen,0,"usage");fwrite(generated+gen.tokens[usage].begin,1,gen.tokens[usage].end-gen.tokens[usage].begin,out);fputs("}\n",out);
    }
    free(source);free(source_version);free(question);free(evidence);free(descriptor);free(count_result);free(generated);free(provider);free(prompt);free(body);free(answer);free(evidence_id);free(session);free(sliced);
    int rc=error?fail(out,error):0;free(provider_error);return rc;
}
int agent_replay(const AgentConfig *cfg,const char *input,size_t length,const char *admitted,const char *history,FILE *out) {
    return agent_replay_session(cfg,input,length,admitted,history,NULL,out);
}
int agent_dispatch(const AgentConfig *cfg,const char *input,size_t length,FILE *out) {
    if(!cfg)return agent_replay(cfg,input,length,NULL,NULL,out);
    AgentConfig received=*cfg;received.received_monotonic=agent_now();
    return agent_replay(&received,input,length,NULL,NULL,out);
}
