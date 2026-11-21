#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 2048
#include "reconcile.h"
#include "command.h"
#include "json.h"
#include "sha256.h"
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const char *version="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static int fake(const char *mode){
    char request[16385];size_t length=fread(request,1,16384,stdin);Json j;
    if(!json_parse(&j,request,length))return 2;
    if(!strcmp(mode,"malformed")){fputs("{}",stdout);return 0;}
    if(!strcmp(mode,"failure")){fputs("{\"contract_version\":1,\"error\":{\"code\":\"scope_unavailable\"}}",stdout);return 0;}
    char scope[65];
    const char *scope_text="directory-scope-v1:local-endpoint:10:20:";
    hash_bytes(scope_text,strlen(scope_text),scope);
    char *operation=json_string(&j,json_get(&j,0,"operation_id"));
    fprintf(stdout,"{\"contract_version\":1,\"operation_id\":\"%s\",\"freshness\":\"observed_checkpoint\",\"result_semantics\":\"committed_observation_history\",\"observation_consistency\":\"per_locator\",\"exhaustive_current_filesystem\":false,\"scope_identity\":\"%s\",\"observations\":[",operation,!strcmp(mode,"scope")?version:scope);
    int array=json_get(&j,0,"locators"),first=1;size_t total=0;
    for(int item=array+1;item<j.tokens[array].next;item=j.tokens[item].next){
        char *path=json_string(&j,json_get(&j,item,"path")),*expected=json_string(&j,json_get(&j,item,"expected_version"));
        size_t device,inode;CHECK(json_uint(&j,json_get(&j,item,"expected_device"),&device));CHECK(json_uint(&j,json_get(&j,item,"expected_inode"),&inode));
        if(!first)fputc(',',stdout);
        first=0;
        Sha256 locator_hash;unsigned char sum[32];char locator_id[65];sha256_init(&locator_hash);
        sha256_update(&locator_hash,"source-locator-v1",sizeof("source-locator-v1"));
        const char *owner=!strcmp(mode,"owner")?"foreign-owner":"local-owner";
        sha256_update(&locator_hash,owner,strlen(owner)+1);sha256_update(&locator_hash,scope,65);
        sha256_update(&locator_hash,path,strlen(path));sha256_final(&locator_hash,sum);sha256_hex(sum,locator_id);
        fprintf(stdout,"{\"locator_id\":\"%s\",\"revision\":1,\"host_id\":\"%s\",\"scope_identity\":\"%s\",\"path\":",locator_id,!strcmp(mode,"host")?"foreign":"local-endpoint",scope);
        json_quote(stdout,!strcmp(mode,"path")?"foreign.txt":path,strlen(!strcmp(mode,"path")?"foreign.txt":path));
        fprintf(stdout,",\"identity_constraint\":\"content_and_inode\",\"expected_version\":\"%s\",\"expected_device\":%zu,\"expected_inode\":%zu,\"status\":\"%s\",\"content_version\":",!strcmp(mode,"version")?scope:expected,device,inode,!strcmp(mode,"missing")?"missing":"available");
        if(!strcmp(mode,"missing"))fputs("null,\"observed_content_hash\":null",stdout);
        else fprintf(stdout,"\"%s\",\"observed_content_hash\":\"%s\"",expected,expected);
        fprintf(stdout,",\"device\":%zu,\"inode\":%zu,\"checked_bytes\":%u,\"observed_at_ms\":123456}",device,!strcmp(mode,"inode")?inode+1:inode,!strcmp(mode,"missing")?0:9);
        total+=!strcmp(mode,"missing")?0:9;free(path);free(expected);
    }
    fprintf(stdout,"],\"checked_bytes\":%zu}",!strcmp(mode,"bytes")?total+1:total);free(operation);return 0;
}
static char *receipt(const char *action,size_t count){
    char *out=NULL;size_t length;FILE *stream=open_memstream(&out,&length);CHECK(stream);
    fprintf(stream,"{\"effect_status\":\"completed\",\"plan\":{\"action\":\"%s\"},\"items\":[",action);
    for(size_t i=0;i<count;i++)fprintf(stream,"%s{\"source\":\"source-%zu.txt\",\"destination\":\"destination-%zu.txt\",\"version\":\"%s\",\"receipt\":{\"published_device\":10,\"published_inode\":%zu,\"removed_device\":10,\"removed_inode\":%zu,\"restored_device\":10,\"restored_inode\":%zu}}",i?",":"",i,i,version,i+40,i+40,i+40);
    fputs("]}",stream);CHECK(!fclose(stream));return out;
}
static char *command(const char *exe,const char *mode){char *out=NULL;size_t length;FILE *stream=open_memstream(&out,&length);CHECK(stream);fputc('[',stream);json_quote(stream,exe,strlen(exe));fputs(",\"--fake\",",stream);json_quote(stream,mode,strlen(mode));fputc(']',stream);CHECK(!fclose(stream));return out;}
static void check(const char *exe,const char *mode,const char *action,size_t count,const char *expected){
    char *cmd=command(exe,mode),*input=receipt(action,count);
    char *out=action_reconcile(cmd,version,"execute","local-endpoint","local-owner",10,20,input);CHECK(out);
    Json j;CHECK(json_parse(&j,out,strlen(out)));
    int reconciliation=json_get(&j,0,"source_reconciliation");char *status=json_string(&j,json_get(&j,reconciliation,"status"));CHECK(status&&!strcmp(status,expected));
    int batches=json_get(&j,reconciliation,"batches");CHECK(batches>=0);
    if(!strcmp(expected,"completed"))CHECK(j.tokens[batches].count==(int)((count*(!strcmp(action,"move")||!strcmp(action,"undo_move")?2:1)+7)/8));
    else CHECK(j.tokens[batches].count==0);
    CHECK(strstr(out,"\"effect_status\":\"completed\""));free(status);free(cmd);free(input);free(out);
}
int main(int argc,char **argv){
    int guardian=command_guardian_dispatch(argc,argv);if(guardian>=0)return guardian;
    if(argc==3&&!strcmp(argv[1],"--fake"))return fake(argv[2]);
    check(argv[0],"valid","copy",1,"completed");check(argv[0],"missing","undo_copy",1,"completed");
    check(argv[0],"valid","move",8,"completed");check(argv[0],"valid","undo_move",8,"completed");
    const char *modes[]={"scope","host","owner","path","version","inode","bytes","malformed","failure"};
    for(size_t i=0;i<sizeof(modes)/sizeof(modes[0]);i++)check(argv[0],modes[i],"copy",1,"failed");
    char *input=receipt("copy",1),*out=action_reconcile(NULL,version,"execute","local-endpoint","local-owner",10,20,input);CHECK(out&&strstr(out,"\"status\":\"unconfigured\""));free(out);
    out=action_reconcile("[\"/nonexistent-samosa-executable\"]",version,"execute","local-endpoint","local-owner",10,20,input);CHECK(out&&strstr(out,"\"status\":\"failed\""));free(out);free(input);
    printf("PASS receipt reconciliation: %u checks; native public transport, qualified failures, two bounded batches\n",checks);return 0;
}
