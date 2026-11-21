#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 2048
#include "reconcile.h"
#include "command.h"
#include "json.h"
#include "sha256.h"
#include <inttypes.h>

typedef struct { char *path,*version; size_t device,inode; } Location;
static int equal(const Json *j,int object,const char *key,const char *expected){
    char *value=json_string(j,json_get(j,object,key));
    int ok=value&&expected&&!strcmp(value,expected);
    free(value); return ok;
}
static int integer(const Json *j,int object,const char *key,size_t expected){
    size_t value; return json_uint(j,json_get(j,object,key),&value)&&value==expected;
}
static void digest(const char *text,char out[65]){
    hash_bytes(text,strlen(text),out);
}
static int valid_response(const char *text,size_t length,const char *operation,
                          const char *scope,const char *host,const char *owner,Location *locations,size_t count){
    Json j;
    if(length>65536||!json_parse(&j,text,length)||j.tokens[0].type!=JT_OBJECT||
       !integer(&j,0,"contract_version",1)||json_get(&j,0,"error")>=0||
       !equal(&j,0,"operation_id",operation)||!equal(&j,0,"scope_identity",scope)||
       !equal(&j,0,"freshness","observed_checkpoint")||
       !equal(&j,0,"result_semantics","committed_observation_history")||
       !equal(&j,0,"observation_consistency","per_locator"))return 0;
    int exhaustive=json_get(&j,0,"exhaustive_current_filesystem");
    if(exhaustive<0||j.tokens[exhaustive].type!=JT_BOOL||
       j.tokens[exhaustive].end-j.tokens[exhaustive].begin!=5||
       memcmp(text+j.tokens[exhaustive].begin,"false",5))return 0;
    int array=json_get(&j,0,"observations");
    if(array<0||j.tokens[array].type!=JT_ARRAY||j.tokens[array].count!=(int)count)return 0;
    int token=array+1; size_t total=0;
    for(size_t i=0;i<count;i++,token=j.tokens[token].next){
        Location *location=&locations[i]; size_t revision,time,checked;
        Sha256 locator_hash;unsigned char sum[32];char expected_id[65];
        sha256_init(&locator_hash);
        sha256_update(&locator_hash,"source-locator-v1",sizeof("source-locator-v1"));
        sha256_update(&locator_hash,owner,strlen(owner)+1);
        sha256_update(&locator_hash,scope,65);
        sha256_update(&locator_hash,location->path,strlen(location->path));
        sha256_final(&locator_hash,sum);sha256_hex(sum,expected_id);
        char *id=json_string(&j,json_get(&j,token,"locator_id"));
        int hash=id&&!strcmp(id,expected_id);
        free(id);
        if(!hash||!equal(&j,token,"scope_identity",scope)||!equal(&j,token,"host_id",host)||
           !equal(&j,token,"path",location->path)||!equal(&j,token,"expected_version",location->version)||
           !equal(&j,token,"identity_constraint","content_and_inode")||
           !integer(&j,token,"expected_device",location->device)||!integer(&j,token,"expected_inode",location->inode)||
           !json_uint(&j,json_get(&j,token,"revision"),&revision)||!revision||
           !json_uint(&j,json_get(&j,token,"observed_at_ms"),&time)||!time||
           !json_uint(&j,json_get(&j,token,"checked_bytes"),&checked)||checked>1048576)return 0;
        total+=checked;
        char *status=json_string(&j,json_get(&j,token,"status"));
        int available=status&&!strcmp(status,"available");
        int qualified=available||(status&&(!strcmp(status,"missing")||!strcmp(status,"changed")||
                         !strcmp(status,"unsupported")||!strcmp(status,"uncertain")));
        free(status);
        if(!qualified)return 0;
        if(available){
            if(!equal(&j,token,"content_version",location->version)||
               !equal(&j,token,"observed_content_hash",location->version)||
               !integer(&j,token,"device",location->device)||!integer(&j,token,"inode",location->inode))return 0;
        }else{
            int version=json_get(&j,token,"content_version");
            if(version<0||j.tokens[version].type!=JT_NULL)return 0;
        }
    }
    return integer(&j,0,"checked_bytes",total);
}

char *action_reconcile(const char *command,const char *plan,const char *operation,
                       const char *host,const char *owner,uint64_t root_device,uint64_t root_inode,const char *result){
    Location locations[16]={{0}}; size_t count=0;
    char *responses[2]={NULL,NULL},*out=NULL; size_t out_length=0;
    const char *reason=NULL,*status="completed";
    Json j;
    if(!command){status="unconfigured";reason="sources_unconfigured";}
    else if(!json_parse(&j,result,strlen(result))||!equal(&j,0,"effect_status","completed")){
        status="pending";reason="effects_incomplete";
    }else{
        int body=json_get(&j,0,"plan"),items=json_get(&j,0,"items");
        char *action=json_string(&j,json_get(&j,body,"action"));
        int move=action&&(!strcmp(action,"move")||!strcmp(action,"undo_move"));
        int undo=action&&(!strcmp(action,"undo_copy")||!strcmp(action,"undo_move"));
        if(!action||items<0||j.tokens[items].type!=JT_ARRAY||j.tokens[items].count<1||j.tokens[items].count>8){status="failed";reason="invalid_receipt";}
        else for(int item=items+1;item<j.tokens[items].next;item=j.tokens[item].next){
            int receipt=json_get(&j,item,"receipt");
            size_t device,inode;
            const char *device_key=undo?(move?"restored_device":"removed_device"):"published_device";
            const char *inode_key=undo?(move?"restored_inode":"removed_inode"):"published_inode";
            if(!json_uint(&j,json_get(&j,receipt,device_key),&device)||
               !json_uint(&j,json_get(&j,receipt,inode_key),&inode)){
                status="failed";reason="invalid_receipt";break;
            }
            for(int which=0;which<(move?2:1);which++){
                Location *location=&locations[count++];
                location->path=json_string(&j,json_get(&j,item,which?"source":"destination"));
                location->version=json_string(&j,json_get(&j,item,"version"));
                location->device=device;location->inode=inode;
                if(!location->path||!location->version){status="failed";reason="invalid_receipt";break;}
            }
            if(reason)break;
        }
        free(action);
    }
    char scope_text[256],scope[65];
    snprintf(scope_text,sizeof(scope_text),"directory-scope-v1:%s:%" PRIu64 ":%" PRIu64 ":",host,root_device,root_inode);
    digest(scope_text,scope);
    if(!reason)for(size_t batch=0;batch<(count+7)/8;batch++){
        char identity[384],hash[65],op[80];
        snprintf(identity,sizeof(identity),"agent-effect-observation-v1:%s:%s:%zu",plan,operation,batch);
        digest(identity,hash);snprintf(op,sizeof(op),"agent-%s",hash);
        char *request=NULL;size_t length=0;
        FILE *stream=open_memstream(&request,&length);
        if(!stream){status="failed";reason="resource_limit";break;}
        fprintf(stream,"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"%s\",\"locators\":[",op);
        size_t first=batch*8,n=count-first;if(n>8)n=8;
        for(size_t i=0;i<n;i++){
            Location *location=&locations[first+i];if(i)fputc(',',stream);
            fputs("{\"path\":",stream);json_quote(stream,location->path,strlen(location->path));
            fputs(",\"expected_version\":",stream);json_quote(stream,location->version,strlen(location->version));
            fprintf(stream,",\"expected_device\":%zu,\"expected_inode\":%zu}",location->device,location->inode);
        }
        fputs("]}",stream);int closed=fclose(stream);
        if(closed||length>16384){free(request);status="failed";reason="request_limit";break;}
        size_t response_length=0;
        const char *error=command_call(command,request,length,10,&responses[batch],&response_length);
        free(request);
        if(error){free(responses[batch]);responses[batch]=NULL;status="failed";reason=error;break;}
        if(!valid_response(responses[batch],response_length,op,scope,host,owner,&locations[first],n)){
            free(responses[batch]);responses[batch]=NULL;
            status="failed";reason="invalid_source_observation";break;
        }
    }
    FILE *stream=open_memstream(&out,&out_length);
    if(stream){
        size_t length=strlen(result);while(length&&isspace((unsigned char)result[length-1]))length--;
        if(length&&result[length-1]=='}'){
            fwrite(result,1,length-1,stream);
            fputs(",\"source_reconciliation\":{\"status\":",stream);json_quote(stream,status,strlen(status));
            fputs(",\"freshness\":\"committed_observation_history\",\"reason\":",stream);
            if(reason)json_quote(stream,reason,strlen(reason));else fputs("null",stream);
            fputs(",\"batches\":[",stream);int first=1;
            for(size_t i=0;i<2;i++)if(responses[i]){if(!first)fputc(',',stream);first=0;fputs(responses[i],stream);}
            fputs("]}}",stream);
        }
        if(fclose(stream)||!out_length){free(out);out=NULL;}
    }
    for(size_t i=0;i<count;i++){free(locations[i].path);free(locations[i].version);}
    for(size_t i=0;i<2;i++)free(responses[i]);
    return out;
}
