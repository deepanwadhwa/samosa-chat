#define _POSIX_C_SOURCE 200809L
#include "evidence.h"
#include "json.h"
static int id_valid(const char *s) {
    if(!s||strlen(s)!=64)return 0;
    for(int i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int envelope(Json *j,const char *text,size_t n) {
    size_t version;
    return json_parse(j,text,n)&&j->tokens[0].type==JT_OBJECT&&json_uint(j,json_get(j,0,"contract_version"),&version)&&version==1;
}
const char *agent_evidence_validate(const char *evidence,size_t evidence_length,const char *source,const char *source_version,size_t start,size_t end,int pdf,char **identity) {
    const char *error=NULL;Json ev={0};char *evidence_id=NULL;const char *start_field=pdf?"start_page":"start_byte",*end_field=pdf?"end_page":"end_byte";
    if(!error) {
        if(!envelope(&ev,evidence,evidence_length))error="invalid_evidence";
        else if(json_get(&ev,0,"error")>=0)error="source_failed";
        else {
            char *actual=json_string(&ev,json_get(&ev,0,"source_id"));char *actual_version=json_string(&ev,json_get(&ev,0,"version"));
            char *kind=json_string(&ev,json_get(&ev,0,"kind"));char *text=json_string(&ev,json_get(&ev,0,"text"));
            evidence_id=json_string(&ev,json_get(&ev,0,"evidence_id"));
            int range=json_get(&ev,0,"range"),coverage=json_get(&ev,0,"coverage"),provenance=json_get(&ev,0,"provenance");size_t lo=0,hi=0,total=0,inspected=0;
            char *status=json_string(&ev,json_get(&ev,coverage,"status"));char *reader=json_string(&ev,json_get(&ev,provenance,"reader"));
            if(!actual||strcmp(actual,source)||!actual_version||strcmp(actual_version,source_version)||!kind||strcmp(kind,"literal")||!id_valid(evidence_id)||
                !json_uint(&ev,json_get(&ev,range,start_field),&lo)||lo!=start||!json_uint(&ev,json_get(&ev,range,end_field),&hi)||hi!=end||
                !json_uint(&ev,json_get(&ev,coverage,pdf?"total_pages":"total_bytes"),&total)||total<end||
                !json_uint(&ev,json_get(&ev,coverage,pdf?"inspected_pages":"inspected_bytes"),&inspected)||inspected!=(pdf?end-start+1:end-start)||
                !status||strcmp(status,pdf?"partial":start==0&&end==total?"complete":"partial")||!reader||strcmp(reader,pdf?"pdfium-text-v1":"text-utf8-v1"))error="invalid_evidence";
            if(!error && !pdf && (!text||strlen(text)!=end-start))error="invalid_evidence";
            if(!error && pdf) {
                char *reason=json_string(&ev,json_get(&ev,coverage,"reason"));int pages=json_get(&ev,0,"pages");
                if(!reason||strcmp(reason,"text_layer_only")||pages<0||ev.tokens[pages].type!=JT_ARRAY||(size_t)ev.tokens[pages].count!=end-start+1)error="invalid_evidence";
                else {
                    size_t number=start;
                    for(int t=pages+1;!error&&t<ev.tokens[pages].next;t=ev.tokens[t].next,number++) {
                        char *page_text=json_string(&ev,json_get(&ev,t,"text"));char *text_status=json_string(&ev,json_get(&ev,t,"text_status"));size_t actual_page;
                        if(!page_text||strlen(page_text)>65536||!json_uint(&ev,json_get(&ev,t,"page"),&actual_page)||actual_page!=number||!text_status||strcmp(text_status,*page_text?"extracted":"empty_text_layer"))error="invalid_evidence";
                        free(page_text);free(text_status);
                    }
                }
                free(reason);
            }
            free(actual);free(actual_version);free(kind);free(text);free(status);free(reader);
        }
    }
    if(error){free(evidence_id);*identity=NULL;}else *identity=evidence_id;return error;
}
