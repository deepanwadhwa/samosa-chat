#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "actions.h"
#include "json.h"
#include "sha256.h"
#include "reconcile.h"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#define FILE_MAX 1048576u
typedef struct {
    sqlite3 *db;
    int directory,lease,root;
    const AgentConfig *cfg;
    const char *owner,*host;
}
Actions;
typedef struct {
    char *source,*destination,*version,*identity,*stage,*status;
    sqlite3_int64 device,inode,stage_device,stage_inode,parent_device,parent_inode;
    sqlite3_int64 source_device,source_inode,source_parent_device,source_parent_inode;
}
Item;
static const char *undo_reconcile(Actions *,const char *,size_t,const Item *);
static const char *undo_commit(Actions *,const char *,size_t,const Item *);
static const char *move_reconcile(Actions *,const char *,size_t,const Item *);
static int fail(FILE *out,const char *code){
    fputs("{\"contract_version\":1,\"error\":{\"code\":",out);
    json_quote(out,code,strlen(code));
    fputs("}}\n",out);
    return 1;
}
static int identity(const char *s,size_t max){
    if(!s||!*s||strlen(s)>max)return 0;
    for(const char *p=s;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p))return 0;
    return 1;
}
static int hash_identity(const char *s){
    if(!s||strlen(s)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int sql(Actions *a,const char *s){
    return sqlite3_exec(a->db,s,NULL,NULL,NULL)==SQLITE_OK;
}
static int prepare(Actions *a,sqlite3_stmt **stmt,const char *s){
    return sqlite3_prepare_v2(a->db,s,-1,stmt,NULL)==SQLITE_OK;
}
static void bind(sqlite3_stmt *stmt,int at,const char *s){
    if(s)sqlite3_bind_text(stmt,at,s,-1,SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt,at);
}
static char *column(sqlite3_stmt *stmt,int at){
    const char *s=(const char*)sqlite3_column_text(stmt,at);
    return s?strdup(s):NULL;
}
static sqlite3_int64 scalar(Actions *a,const char *s){
    sqlite3_stmt *stmt=NULL;
    sqlite3_int64 n=-1;
    if(prepare(a,&stmt,s)&&sqlite3_step(stmt)==SQLITE_ROW)n=sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt);
    return n;
}
static void release(Actions *a){
    if(a->db)sqlite3_close(a->db);
    if(a->lease>=0)close(a->lease);
    if(a->root>=0)close(a->root);
    if(a->directory>=0)close(a->directory);
}
static const char *source_scope_columns(Actions *a){
    const char *names[]={
        "source_device","source_inode","source_parent_device","source_parent_inode"
    }
    ;
    int present[4]={
        0
    }
    ;
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"PRAGMA table_info(copy_items)"))return "store_failed";
    int rc;
    while((rc=sqlite3_step(stmt))==SQLITE_ROW){
        const char *name=(const char*)sqlite3_column_text(stmt,1);
        for(size_t i=0;i<4;i++)if(name&&!strcmp(name,names[i]))present[i]=1;
    }
    sqlite3_finalize(stmt);
    if(rc!=SQLITE_DONE)return "store_failed";
    for(size_t i=0;i<4;i++)if(!present[i]){
        char statement[128];
        snprintf(statement,sizeof(statement),"ALTER TABLE copy_items ADD COLUMN %s INTEGER NOT NULL DEFAULT 0",names[i]);
        if(!sql(a,statement))return "store_failed";
    }
    return NULL;
}
static const char *open_actions(Actions *a,const AgentConfig *cfg){
    memset(a,0,sizeof(*a));
    a->directory=a->lease=a->root=-1;
    a->cfg=cfg;
    a->owner=cfg&&cfg->owner?cfg->owner:"local-owner";
    a->host=cfg&&cfg->action_host_id?cfg->action_host_id:"local-endpoint";
    if(!cfg||!cfg->state||!identity(a->owner,128)||!identity(a->host,128))return "invalid_configuration";
    if(mkdir(cfg->state,0700)&&errno!=EEXIST)return "state_unavailable";
    a->directory=open(cfg->state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;
    if(a->directory<0||fstat(a->directory,&info)||info.st_uid!=geteuid()||(info.st_mode&077))return "state_unavailable";
    int fd=openat(a->directory,"actions.sqlite3",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(fd<0)return "state_unavailable";
    int safe=!fstat(fd,&info)&&S_ISREG(info.st_mode)&&info.st_uid==geteuid()&&!(info.st_mode&077)&&info.st_nlink==1;
    close(fd);
    if(!safe)return "state_unavailable";
    size_t n=strlen(cfg->state)+32;
    char *path=malloc(n);
    if(!path)return "resource_limit";
    snprintf(path,n,"%s/actions.sqlite3",cfg->state);
    int rc=sqlite3_open_v2(path,&a->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);
    free(path);
    if(rc!=SQLITE_OK)return "state_unavailable";
    sqlite3_busy_timeout(a->db,2000);
    sqlite3_limit(a->db,SQLITE_LIMIT_LENGTH,1048576);
    if(!sql(a,"PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;PRAGMA max_page_count=4096;CREATE TABLE IF NOT EXISTS plans(id TEXT PRIMARY KEY,owner TEXT NOT NULL,body TEXT NOT NULL,root_device INTEGER NOT NULL,root_inode INTEGER NOT NULL,approved INTEGER NOT NULL DEFAULT 0);CREATE TABLE IF NOT EXISTS copy_items(plan TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,destination TEXT NOT NULL,version TEXT NOT NULL,identity TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'pending',parent_device INTEGER NOT NULL,parent_inode INTEGER NOT NULL,stage TEXT,stage_device INTEGER,stage_inode INTEGER,payload_device INTEGER,payload_inode INTEGER,error TEXT,reversed INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(plan,ordinal));CREATE TABLE IF NOT EXISTS action_operations(owner TEXT NOT NULL,operation TEXT NOT NULL,request TEXT NOT NULL,plan TEXT NOT NULL,status TEXT NOT NULL,result TEXT,PRIMARY KEY(owner,operation));"))return "store_failed";
    sqlite3_stmt *columns=NULL;
    int reversed=0;
    if(!prepare(a,&columns,"PRAGMA table_info(copy_items)"))return "store_failed";
    while(sqlite3_step(columns)==SQLITE_ROW){
        const char *name=(const char*)sqlite3_column_text(columns,1);
        if(name&&!strcmp(name,"reversed"))reversed=1;
    }
    sqlite3_finalize(columns);
    if(!reversed&&!sql(a,"ALTER TABLE copy_items ADD COLUMN reversed INTEGER NOT NULL DEFAULT 0"))return "store_failed";
    return source_scope_columns(a);
}
static const char *lock_actions(Actions *a){
    a->lease=openat(a->directory,"actions.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    struct stat info;
    if(a->lease<0||fstat(a->lease,&info)||!S_ISREG(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&077)||info.st_nlink!=1)return "state_unavailable";
    return flock(a->lease,LOCK_EX|LOCK_NB)?"busy":NULL;
}
static int path_valid(const char *s){
    if(!s||!*s||strlen(s)>4096||s[0]=='/'||s[strlen(s)-1]=='/')return 0;
    const char *p=s;
    for(;;){
        const char *slash=strchr(p,'/');
        size_t n=slash?(size_t)(slash-p):strlen(p);
        if(!n||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.'))return 0;
        if(!slash)break;
        p=slash+1;
    }
    return 1;
}
static int parent(Actions *a,const char *path,char **leaf){
    *leaf=NULL;
    if(a->root<0||!path_valid(path))return -1;
    char *copy=strdup(path);
    if(!copy)return -1;
    int fd=dup(a->root);
    if(fd<0){
        free(copy);
        return -1;
    }
    char *p=copy;
    for(;;){
        char *slash=strchr(p,'/');
        if(!slash){
            *leaf=strdup(p);
            break;
        }
        *slash=0;
        int next=openat(fd,p,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd);
        fd=next;
        if(fd<0)break;
        p=slash+1;
    }
    free(copy);
    if(!*leaf&&fd>=0){
        close(fd);
        fd=-1;
    }
    return fd;
}
static const char *open_root(Actions *a,sqlite3_int64 device,sqlite3_int64 inode){
    if(a->root>=0){
        close(a->root);
        a->root=-1;
    }
    if(!a->cfg->action_root)return "action_root_unavailable";
    a->root=open(a->cfg->action_root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;
    if(a->root<0||fstat(a->root,&info))return "action_root_unavailable";
    if(device&&((sqlite3_int64)info.st_dev!=device||(sqlite3_int64)info.st_ino!=inode))return "stale_scope";
    return NULL;
}
static void metadata_identity(const struct stat *s,char id[65]){
#ifdef __APPLE__
    long nanos=s->st_mtimespec.tv_nsec;
#else
    long nanos=s->st_mtim.tv_nsec;
#endif
    char value[256];
    int n=snprintf(value,sizeof(value),"file-identity-v1:%llu:%llu:%llu:%lld:%ld",(unsigned long long)s->st_dev,(unsigned long long)s->st_ino,(unsigned long long)s->st_size,(long long)s->st_mtime,nanos);
    hash_bytes(value,(size_t)n,id);
}
static int stable(const struct stat *a,const struct stat *b){
    char x[65],y[65];
    metadata_identity(a,x);
    metadata_identity(b,y);
#ifdef __APPLE__
    int ctime_same=a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec&&a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    int ctime_same=a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
    return !strcmp(x,y)&&ctime_same;
}
static const char *copy_bytes(int source,int output,const char *expected,const char *expected_identity,struct stat *captured){
    struct stat before,after;
    if(fstat(source,&before)||!S_ISREG(before.st_mode)||before.st_size<0||(unsigned long long)before.st_size>FILE_MAX)return "invalid_source";
    char actual[65];
    metadata_identity(&before,actual);
    if(expected_identity&&strcmp(actual,expected_identity))return "stale_source";
    unsigned char buffer[65536],sum[32];
    Sha256 hash;
    sha256_init(&hash);
    size_t count=0;
    for(;;){
        ssize_t n=read(source,buffer,sizeof(buffer));
        if(n<0){
            if(errno==EINTR)continue;
            return "source_failed";
        }
        if(!n)break;
        if((size_t)n>FILE_MAX-count)return "source_limit";
        count+=(size_t)n;
        sha256_update(&hash,buffer,(size_t)n);
        if(output>=0){
            size_t offset=0;
            while(offset<(size_t)n){
                ssize_t written=write(output,buffer+offset,(size_t)n-offset);
                if(written<0&&errno==EINTR)continue;
                if(written<=0)return "copy_failed";
                offset+=(size_t)written;
            }
        }
    }
    if(fstat(source,&after)||!stable(&before,&after)||count!=(size_t)before.st_size)return "stale_source";
    sha256_final(&hash,sum);
    sha256_hex(sum,actual);
    if(strcmp(actual,expected))return "stale_source";
    if(captured)*captured=before;
    return NULL;
}
static int plan_reply(Actions *a,const char *plan,FILE *out){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"SELECT body,approved,(SELECT count(*) FROM copy_items WHERE plan=plans.id),(SELECT count(*) FROM copy_items WHERE plan=plans.id AND status='copied'),(SELECT count(*) FROM copy_items WHERE plan=plans.id AND status='failed'),(SELECT count(*) FROM copy_items WHERE plan=plans.id AND status IN ('intent','prepared','undo_prepared','undo_captured','undo_removing','move_prepared','move_captured')),(SELECT count(*) FROM copy_items WHERE plan=plans.id AND status='undone'),(SELECT count(*) FROM copy_items WHERE plan=plans.id AND status='moved') FROM plans WHERE id=? AND owner=?"))return fail(out,"store_failed");
    bind(stmt,1,plan);
    bind(stmt,2,a->owner);
    int rc=sqlite3_step(stmt);
    if(rc!=SQLITE_ROW){
        sqlite3_finalize(stmt);
        return fail(out,rc==SQLITE_DONE?"unknown_plan":"store_failed");
    }
    const char *body=(const char*)sqlite3_column_text(stmt,0);
    Json j;
    if(!body||!json_parse(&j,body,strlen(body))){
        sqlite3_finalize(stmt);
        return fail(out,"corrupt_plan");
    }
    sqlite3_int64 total=sqlite3_column_int64(stmt,2),copied=sqlite3_column_int64(stmt,3),failed=sqlite3_column_int64(stmt,4),unreconciled=sqlite3_column_int64(stmt,5),undone=sqlite3_column_int64(stmt,6),moved=sqlite3_column_int64(stmt,7);
    char *action=json_string(&j,json_get(&j,0,"action"));
    int restoring=action&&!strcmp(action,"undo_move");
    free(action);
    fprintf(out,"{\"contract_version\":1,\"plan_id\":\"%s\",\"plan_revision\":1,\"approved\":%s,\"total_items\":%lld,\"copied_items\":%lld,\"undone_items\":%lld,\"moved_items\":%lld,\"failed_items\":%lld,\"unreconciled_items\":%lld,\"effect_status\":\"%s\",\"receipt_freshness\":\"committed_history\",\"plan\":%s,\"items\":[",plan,sqlite3_column_int(stmt,1)?"true":"false",(long long)total,(long long)copied,(long long)undone,(long long)moved,(long long)failed,(long long)unreconciled,copied+undone+moved==total&&!unreconciled?"completed":copied||undone||moved||failed?"partial":"pending",body);
    sqlite3_finalize(stmt);
    stmt=NULL;
    if(!prepare(a,&stmt,"SELECT ordinal,source,destination,version,status,error,payload_device,payload_inode,reversed FROM copy_items WHERE plan=? ORDER BY ordinal LIMIT 8"))return 1;
    bind(stmt,1,plan);
    int first=1;
    while((rc=sqlite3_step(stmt))==SQLITE_ROW){
        if(!first)fputc(',',out);
        first=0;
        fprintf(out,"{\"ordinal\":%lld",(long long)sqlite3_column_int64(stmt,0));
        const char *keys[]={
            "source","destination","version","status","error"
        }
        ;
        for(size_t i=0;i<5;i++){
            const char *value=(const char*)sqlite3_column_text(stmt,(int)i+1);
            fputc(',',out);
            json_quote(out,keys[i],strlen(keys[i]));
            fputc(':',out);
            if(value)json_quote(out,value,strlen(value));
            else fputs("null",out);
        }
        if(!strcmp((const char*)sqlite3_column_text(stmt,4),"copied")||!strcmp((const char*)sqlite3_column_text(stmt,4),"moved"))fprintf(out,",\"receipt\":{\"published_device\":%lld,\"published_inode\":%lld}",(long long)sqlite3_column_int64(stmt,6),(long long)sqlite3_column_int64(stmt,7));
        if(!strcmp((const char*)sqlite3_column_text(stmt,4),"undone"))fprintf(out,",\"receipt\":{\"%s_device\":%lld,\"%s_inode\":%lld}",restoring?"restored":"removed",(long long)sqlite3_column_int64(stmt,6),restoring?"restored":"removed",(long long)sqlite3_column_int64(stmt,7));
        fprintf(out,",\"reversed\":%s",sqlite3_column_int(stmt,8)?"true":"false");
        fputc('}',out);
    }
    sqlite3_finalize(stmt);
    fputs("]}\n",out);
    return rc==SQLITE_DONE?0:1;
}
static char *plan_buffer(Actions *a,const char *id){
    char *out=NULL;
    size_t n;
    FILE *stream=open_memstream(&out,&n);
    if(!stream)return NULL;
    int rc=plan_reply(a,id,stream);
    int closed=fclose(stream);
    if(rc||closed){
        free(out);
        return NULL;
    }
    return out;
}
static const char *retry(Actions *a,const char *operation,const char *request,int *found){
    *found=0;
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"SELECT request,status FROM action_operations WHERE owner=? AND operation=?"))return "store_failed";
    bind(stmt,1,a->owner);
    bind(stmt,2,operation);
    int rc=sqlite3_step(stmt);
    const char *error=NULL;
    if(rc==SQLITE_ROW){
        const char *status=(const char*)sqlite3_column_text(stmt,1);
        *found=status&&!strcmp(status,"running")?2:1;
        const char *old=(const char*)sqlite3_column_text(stmt,0);
        if(!old||strcmp(old,request))error="idempotency_conflict";
    }
    else if(rc!=SQLITE_DONE)error="store_failed";
    sqlite3_finalize(stmt);
    return error;
}
static int operation_reply(Actions *a,const char *operation,FILE *out){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"SELECT plan,status,result FROM action_operations WHERE owner=? AND operation=?"))return fail(out,"store_failed");
    bind(stmt,1,a->owner);
    bind(stmt,2,operation);
    int rc=sqlite3_step(stmt);
    if(rc!=SQLITE_ROW){
        sqlite3_finalize(stmt);
        return fail(out,"unknown_operation");
    }
    fprintf(out,"{\"contract_version\":1,\"operation_id\":");
    json_quote(out,operation,strlen(operation));
    fprintf(out,",\"plan_id\":\"%s\",\"status\":\"%s\",\"result\":%s}\n",(const char*)sqlite3_column_text(stmt,0),(const char*)sqlite3_column_text(stmt,1),sqlite3_column_text(stmt,2)?(const char*)sqlite3_column_text(stmt,2):"null");
    sqlite3_finalize(stmt);
    return 0;
}
static const char *admit(Actions *a,const char *operation,const char *request,const char *plan){
    if(scalar(a,"SELECT count(*) FROM action_operations")>=128)return "action_limit";
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"INSERT INTO action_operations(owner,operation,request,plan,status) VALUES(?,?,?,?,'running')"))return "store_failed";
    bind(stmt,1,a->owner);
    bind(stmt,2,operation);
    bind(stmt,3,request);
    bind(stmt,4,plan);
    int rc=sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE?NULL:"store_failed";
}
static const char *finish(Actions *a,const char *operation,const char *result){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"UPDATE action_operations SET status='completed',result=? WHERE owner=? AND operation=?"))return "store_failed";
    bind(stmt,1,result);
    bind(stmt,2,a->owner);
    bind(stmt,3,operation);
    int rc=sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE?NULL:"store_failed";
}
static void item_free(Item *item){
    free(item->source);
    free(item->destination);
    free(item->version);
    free(item->identity);
    free(item->stage);
    free(item->status);
    memset(item,0,sizeof(*item));
}
static int fields_allowed(const Json *j,int object,const char *allowed){
    if(object<0||j->tokens[object].type!=JT_OBJECT)return 0;
    for(int t=object+1;t<j->tokens[object].next;t=j->tokens[j->tokens[t].next].next){
        char *key=json_string(j,t),field[128];
        int n=snprintf(field,sizeof(field),"|%s|",key?key:"");
        free(key);
        if(n<0||(size_t)n>=sizeof(field)||!strstr(allowed,field))return 0;
    }
    return 1;
}
static char *normalize_plan(const Json *request,const char **error){
    char *host=json_string(request,json_get(request,0,"host_id")),*action=json_string(request,json_get(request,0,"action"));
    int entries=json_get(request,0,"files");
    char *out=NULL;
    size_t n;
    FILE *stream=NULL;
    if(!identity(host,128)||!action||(strcmp(action,"copy")&&strcmp(action,"move"))||entries<0||request->tokens[entries].type!=JT_ARRAY||!request->tokens[entries].count||request->tokens[entries].count>8)*error="invalid_plan";
    if(!*error){
        stream=open_memstream(&out,&n);
        if(!stream)*error="resource_limit";
        else {
            fputs("{\"action\":",stream);
            json_quote(stream,action,strlen(action));
            fputs(",\"host_id\":",stream);
            json_quote(stream,host,strlen(host));
            fputs(",\"files\":[",stream);
        }
    }
    const char *keys[]={
        "source","destination","version","file_identity"
    }
    ;
    size_t ordinal=0;
    for(int t=entries+1;!*error&&t<request->tokens[entries].next;t=request->tokens[t].next,ordinal++){
        if(!fields_allowed(request,t,"|source|destination|version|file_identity|")){
            *error="invalid_plan";
            break;
        }
        char *values[4]={
            0
        }
        ;
        for(size_t i=0;i<4;i++)values[i]=json_string(request,json_get(request,t,keys[i]));
        if(!path_valid(values[0])||!path_valid(values[1])||!strcmp(values[0],values[1])||!hash_identity(values[2])||!hash_identity(values[3]))*error="invalid_plan";
        for(int prior=entries+1;!*error&&prior<t;prior=request->tokens[prior].next){
            char *destination=json_string(request,json_get(request,prior,"destination"));
            if(destination&&!strcmp(destination,values[1]))*error="destination_conflict";
            free(destination);
        }
        if(!*error){
            if(ordinal)fputc(',',stream);
            fputc('{',stream);
            for(size_t i=0;i<4;i++){
                if(i)fputc(',',stream);
                json_quote(stream,keys[i],strlen(keys[i]));
                fputc(':',stream);
                json_quote(stream,values[i],strlen(values[i]));
            }
            fputc('}',stream);
        }
        for(size_t i=0;i<4;i++)free(values[i]);
    }
    if(stream){
        fputs("]}",stream);
        if(fclose(stream))*error="resource_limit";
    }
    free(host);
    free(action);
    if(*error){
        free(out);
        return NULL;
    }
    return out;
}
static char *normalize_operation(const char *op,const char *operation,const char *plan,const char *body){
    char *out=NULL;
    size_t n;
    FILE *stream=open_memstream(&out,&n);
    if(!stream)return NULL;
    fputs("{\"op\":",stream);
    json_quote(stream,op,strlen(op));
    fputs(",\"operation_id\":",stream);
    json_quote(stream,operation,strlen(operation));
    if(plan)fprintf(stream,",\"plan_id\":\"%s\",\"plan_revision\":1",plan);
    if(body){
        fputs(",\"body\":",stream);
        fputs(body,stream);
    }
    fputc('}',stream);
    if(fclose(stream)){
        free(out);
        return NULL;
    }
    return out;
}
static const char *root_for_plan(Actions *a,const char *plan,int require_approval){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"SELECT root_device,root_inode,approved,body FROM plans WHERE id=? AND owner=?"))return "store_failed";
    bind(stmt,1,plan);
    bind(stmt,2,a->owner);
    int rc=sqlite3_step(stmt);
    const char *error=NULL;
    sqlite3_int64 device=0,inode=0;
    if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_plan":"store_failed";
    else if(require_approval&&!sqlite3_column_int(stmt,2))error="approval_required";
    else {
        device=sqlite3_column_int64(stmt,0);
        inode=sqlite3_column_int64(stmt,1);
        const char *body=(const char*)sqlite3_column_text(stmt,3);
        Json j;
        if(!body||!json_parse(&j,body,strlen(body)))error="corrupt_plan";
        else {
            char *host=json_string(&j,json_get(&j,0,"host_id"));
            if(!host||strcmp(host,a->host))error="wrong_host";
            free(host);
        }
    }
    sqlite3_finalize(stmt);
    if(!error){
        if(a->root>=0){
            close(a->root);
            a->root=-1;
        }
        error=open_root(a,device,inode);
    }
    return error;
}
static const char *item_status(Actions *a,const char *plan,size_t ordinal,const char *status,const char *error){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"UPDATE copy_items SET status=?,error=? WHERE plan=? AND ordinal=?"))return "store_failed";
    bind(stmt,1,status);
    bind(stmt,2,error);
    bind(stmt,3,plan);
    sqlite3_bind_int64(stmt,4,(sqlite3_int64)ordinal);
    int rc=sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE?NULL:"store_failed";
}
static int load_item(Actions *a,const char *plan,size_t ordinal,Item *item){
    sqlite3_stmt *stmt=NULL;
    memset(item,0,sizeof(*item));
    if(!prepare(a,&stmt,"SELECT source,destination,version,identity,stage,status,payload_device,payload_inode,stage_device,stage_inode,parent_device,parent_inode,source_device,source_inode,source_parent_device,source_parent_inode FROM copy_items WHERE plan=? AND ordinal=?"))return 0;
    bind(stmt,1,plan);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);
    int rc=sqlite3_step(stmt);
    if(rc==SQLITE_ROW){
        item->source=column(stmt,0);
        item->destination=column(stmt,1);
        item->version=column(stmt,2);
        item->identity=column(stmt,3);
        item->stage=column(stmt,4);
        item->status=column(stmt,5);
        item->device=sqlite3_column_int64(stmt,6);
        item->inode=sqlite3_column_int64(stmt,7);
        item->stage_device=sqlite3_column_int64(stmt,8);
        item->stage_inode=sqlite3_column_int64(stmt,9);
        item->parent_device=sqlite3_column_int64(stmt,10);
        item->parent_inode=sqlite3_column_int64(stmt,11);
        item->source_device=sqlite3_column_int64(stmt,12);
        item->source_inode=sqlite3_column_int64(stmt,13);
        item->source_parent_device=sqlite3_column_int64(stmt,14);
        item->source_parent_inode=sqlite3_column_int64(stmt,15);
    }
    sqlite3_finalize(stmt);
    return rc==SQLITE_ROW&&item->source&&item->destination&&item->version&&item->identity&&item->status;
}
static int anchors_open(Actions *a){
    if(mkdirat(a->directory,"action-anchors",0700)&&errno!=EEXIST)return -1;
    int fd=openat(a->directory,"action-anchors",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;
    if(fd<0)return -1;
    if(fstat(fd,&info)||info.st_uid!=geteuid()||(info.st_mode&077)){
        close(fd);
        return -1;
    }
    return fd;
}
static void anchor_name(const char *plan,size_t ordinal,char name[80]){
    snprintf(name,80,"%s-%zu",plan,ordinal);
}
static const char *anchor_create(Actions *a,const char *plan,size_t ordinal,int stage){
    int anchors=anchors_open(a);
    if(anchors<0)return "state_unavailable";
    char name[80];
    anchor_name(plan,ordinal,name);
    const char *error=NULL;
    if(linkat(stage,"payload",anchors,name,0))error=errno==EXDEV?"cross_device_anchor":errno==EEXIST?"anchor_conflict":"copy_failed";
    else if(fsync(anchors))error="receipt_pending";
    close(anchors);
    return error;
}
static const char *anchor_verify(Actions *a,const char *plan,size_t ordinal,const Item *item){
    int anchors=anchors_open(a);
    if(anchors<0)return "state_unavailable";
    char name[80];
    anchor_name(plan,ordinal,name);
    int fd=openat(anchors,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    struct stat info;
    const char *error=NULL;
    if(fd<0||fstat(fd,&info)||!S_ISREG(info.st_mode)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)error="undo_anchor_unavailable";
    else error=copy_bytes(fd,-1,item->version,NULL,NULL);
    if(fd>=0)close(fd);
    close(anchors);
    return error;
}
static void cleanup_stage(int directory,const Item *item){
    if(!item->stage)return;
    int stage=openat(directory,item->stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(stage<0)return;
    struct stat info,payload,named;
    if(!fstat(stage,&info)&&info.st_uid==geteuid()&&!(info.st_mode&077)&&(sqlite3_int64)info.st_dev==item->stage_device&&(sqlite3_int64)info.st_ino==item->stage_inode&& !fstatat(stage,"payload",&payload,AT_SYMLINK_NOFOLLOW)&&S_ISREG(payload.st_mode)&&(sqlite3_int64)payload.st_dev==item->device&&(sqlite3_int64)payload.st_ino==item->inode&& !unlinkat(stage,"payload",0)&&!fstatat(directory,item->stage,&named,AT_SYMLINK_NOFOLLOW)&&named.st_dev==info.st_dev&&named.st_ino==info.st_ino){
        (void)unlinkat(directory,item->stage,AT_REMOVEDIR);
        (void)fsync(directory);
    }
    close(stage);
}
static const char *reconcile(Actions *a,const char *plan,size_t ordinal,const Item *item){
    char *leaf=NULL;
    int directory=parent(a,item->destination,&leaf);
    struct stat parent_info;
    if(directory<0||fstat(directory,&parent_info)||(sqlite3_int64)parent_info.st_dev!=item->parent_device||(sqlite3_int64)parent_info.st_ino!=item->parent_inode){
        if(directory>=0)close(directory);
        free(leaf);
        return "destination_changed";
    }
    int fd=openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    const char *error=NULL;
    struct stat info;
    if(fd<0){
        if(errno!=ENOENT)error="destination_changed";
    }
    else if(fstat(fd,&info)||!S_ISREG(info.st_mode)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)error="destination_changed";
    else {
        error=copy_bytes(fd,-1,item->version,NULL,NULL);
        if(!error)error=item_status(a,plan,ordinal,"copied",NULL);
        if(!error)cleanup_stage(directory,item);
    }
    if(fd>=0)close(fd);
    close(directory);
    free(leaf);
    return error;
}
static const char *recover(Actions *a){
    if(!sql(a,"BEGIN IMMEDIATE"))return "store_busy";
    sqlite3_stmt *stmt=NULL;
    const char *error=NULL;
    if(!prepare(a,&stmt,"UPDATE action_operations SET status='interrupted',result='{\"error\":{\"code\":\"interrupted\"}}' WHERE owner=? AND status='running'"))error="store_failed";
    else {
        bind(stmt,1,a->owner);
        if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);
    stmt=NULL;
    if(error)sql(a,"ROLLBACK");
    else if(!sql(a,"COMMIT")){
        sql(a,"ROLLBACK");
        error="store_failed";
    }
    if(error||!a->cfg->action_root)return error;
    char *plans[256]={
        0
    }
    ;
    size_t ordinals[256],count=0;
    if(!prepare(a,&stmt,"SELECT copy_items.plan,ordinal FROM copy_items JOIN plans ON plans.id=copy_items.plan WHERE owner=? AND status IN ('prepared','undo_prepared','undo_captured','undo_removing','move_prepared','move_captured') ORDER BY plan,ordinal LIMIT 256"))error="store_failed";
    else {
        bind(stmt,1,a->owner);
        for(;;){
            int rc=sqlite3_step(stmt);
            if(rc==SQLITE_DONE)break;
            if(rc!=SQLITE_ROW||count>=256){
                error="store_failed";
                break;
            }
            plans[count]=column(stmt,0);
            ordinals[count]=(size_t)sqlite3_column_int64(stmt,1);
            if(!plans[count]){
                error="resource_limit";
                break;
            }
            count++;
        }
    }
    sqlite3_finalize(stmt);
    /* Receipt inspection never publishes a missing destination. */ for(size_t i=0;!error&&i<count;i++){
        error=root_for_plan(a,plans[i],0);
        if(error&&(!strcmp(error,"stale_scope")||!strcmp(error,"wrong_host")||!strcmp(error,"action_root_unavailable"))){
            error=NULL;
            continue;
        }
        Item item={
            0
        }
        ;
        if(!error&&!load_item(a,plans[i],ordinals[i],&item))error="corrupt_plan";
        if(!error)error=!strncmp(item.status,"move_",5)?move_reconcile(a,plans[i],ordinals[i],&item):!strncmp(item.status,"undo_",5)?undo_reconcile(a,plans[i],ordinals[i],&item):reconcile(a,plans[i],ordinals[i],&item);
        if(error&&(!strcmp(error,"destination_changed")||!strcmp(error,"stale_source"))){
            if(!strncmp(item.status,"move_",5)||!strncmp(item.status,"undo_",5))error="needs_reconciliation";
            else error=item_status(a,plans[i],ordinals[i],"failed","destination_changed");
        }
        if(error&&!strcmp(error,"needs_reconciliation"))error=item_status(a,plans[i],ordinals[i],item.status,"needs_reconciliation");
        item_free(&item);
    }
    for(size_t i=0;i<count;i++)free(plans[i]);
    return error;
}
static int create_plan(Actions *a,const Json *request,FILE *out){
    const char *error=NULL;
    char *body=normalize_plan(request,&error),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*result=NULL;
    int found=0,transaction=0;
    char plan[65]={
        0
    }
    ;
    if(!identity(operation,128))error="invalid_request";
    if(!error){
        encoded=normalize_operation("plan_create",operation,NULL,body);
        if(!encoded)error="resource_limit";
        else error=retry(a,operation,encoded,&found);
    }
    if(!error&&found==1)goto done;
    if(!error){
        error=lock_actions(a);
        if(error&&found==2&&!strcmp(error,"busy")){
            error=NULL;
            goto done;
        }
    }
    if(!error)error=recover(a);
    if(!error)error=retry(a,operation,encoded,&found);
    if(!error&&found)goto done;
    if(!error){
        sqlite3_int64 plans=scalar(a,"SELECT count(*) FROM plans"),operations=scalar(a,"SELECT count(*) FROM action_operations");
        if(plans<0||operations<0)error="store_failed";
        else if(plans>=32||operations>=128)error="action_limit";
    }
    Json j;
    int entries=-1;
    struct stat root={
        0
    }
    ;
    if(!error){
        if(!json_parse(&j,body,strlen(body)))error="invalid_plan";
        else {
            char *host=json_string(&j,json_get(&j,0,"host_id"));
            if(!host||strcmp(host,a->host))error="wrong_host";
            free(host);
            entries=json_get(&j,0,"files");
        }
    }
    if(!error)error=open_root(a,0,0);
    if(!error&&fstat(a->root,&root))error="action_root_unavailable";
    for(int t=entries+1;!error&&t<j.tokens[entries].next;t=j.tokens[t].next){
        char *source=json_string(&j,json_get(&j,t,"source")),*destination=json_string(&j,json_get(&j,t,"destination")),*version=json_string(&j,json_get(&j,t,"version")),*id=json_string(&j,json_get(&j,t,"file_identity")),*leaf=NULL;
        int directory=parent(a,source,&leaf),fd=directory<0?-1:openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(fd<0)error="source_unavailable";
        else error=copy_bytes(fd,-1,version,id,NULL);
        if(fd>=0)close(fd);
        if(directory>=0)close(directory);
        free(leaf);
        leaf=NULL;
        directory=parent(a,destination,&leaf);
        if(!error){
            struct stat info;
            if(directory<0)error="destination_unavailable";
            else if(!fstatat(directory,leaf,&info,AT_SYMLINK_NOFOLLOW))error="destination_exists";
            else if(errno!=ENOENT)error="destination_unavailable";
        }
        if(directory>=0)close(directory);
        free(leaf);
        free(source);
        free(destination);
        free(version);
        free(id);
    }
    if(!error){
        char root_value[128];
        int n=snprintf(root_value,sizeof(root_value),"%llu:%llu",(unsigned long long)root.st_dev,(unsigned long long)root.st_ino);
        Sha256 hash;
        unsigned char sum[32];
        sha256_init(&hash);
        sha256_update(&hash,a->owner,strlen(a->owner)+1);
        sha256_update(&hash,encoded,strlen(encoded));
        sha256_update(&hash,root_value,(size_t)n);
        sha256_final(&hash,sum);
        sha256_hex(sum,plan);
        if(!sql(a,"BEGIN IMMEDIATE"))error="store_busy";
        else transaction=1;
    }
    sqlite3_stmt *stmt=NULL;
    if(!error){
        if(!prepare(a,&stmt,"INSERT INTO plans(id,owner,body,root_device,root_inode) VALUES(?,?,?,?,?)"))error="store_failed";
        else {
            bind(stmt,1,plan);
            bind(stmt,2,a->owner);
            bind(stmt,3,body);
            sqlite3_bind_int64(stmt,4,(sqlite3_int64)root.st_dev);
            sqlite3_bind_int64(stmt,5,(sqlite3_int64)root.st_ino);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    size_t ordinal=0;
    for(int t=entries+1;!error&&t<j.tokens[entries].next;t=j.tokens[t].next,ordinal++){
        char *source=json_string(&j,json_get(&j,t,"source")),*destination=json_string(&j,json_get(&j,t,"destination")),*version=json_string(&j,json_get(&j,t,"version")),*id=json_string(&j,json_get(&j,t,"file_identity"));
        if(!prepare(a,&stmt,"INSERT INTO copy_items(plan,ordinal,source,destination,version,identity,parent_device,parent_inode,source_device,source_inode,source_parent_device,source_parent_inode) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"))error="store_failed";
        else {
            bind(stmt,1,plan);
            sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);
            bind(stmt,3,source);
            bind(stmt,4,destination);
            bind(stmt,5,version);
            bind(stmt,6,id);
            char *leaf=NULL;
            int directory=parent(a,destination,&leaf);
            struct stat info;
            if(directory<0||fstat(directory,&info))error="destination_unavailable";
            else {
                sqlite3_bind_int64(stmt,7,(sqlite3_int64)info.st_dev);
                sqlite3_bind_int64(stmt,8,(sqlite3_int64)info.st_ino);
            }
            if(directory>=0)close(directory);
            free(leaf);
            leaf=NULL;
            directory=parent(a,source,&leaf);
            int source_fd=directory<0?-1:openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            struct stat captured,source_parent,named;
            if(!error){
                if(source_fd<0||fstat(directory,&source_parent))error="source_unavailable";
                else error=copy_bytes(source_fd,-1,version,id,&captured);
            }
            if(!error&&(fstatat(directory,leaf,&named,AT_SYMLINK_NOFOLLOW)||!stable(&captured,&named)))error="stale_source";
            if(!error){
                sqlite3_bind_int64(stmt,9,(sqlite3_int64)captured.st_dev);
                sqlite3_bind_int64(stmt,10,(sqlite3_int64)captured.st_ino);
                sqlite3_bind_int64(stmt,11,(sqlite3_int64)source_parent.st_dev);
                sqlite3_bind_int64(stmt,12,(sqlite3_int64)source_parent.st_ino);
            }
            if(source_fd>=0)close(source_fd);
            if(directory>=0)close(directory);
            free(leaf);
            if(!error&&sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
        free(source);
        free(destination);
        free(version);
        free(id);
    }
    if(!error)error=admit(a,operation,encoded,plan);
    if(!error){
        result=plan_buffer(a,plan);
        if(!result)error="store_failed";
        if(!error)error=finish(a,operation,result);
    }
    if(transaction){
        if(error)sql(a,"ROLLBACK");
        else if(!sql(a,"COMMIT")){
            sql(a,"ROLLBACK");
            error="store_failed";
        }
    }
    done: free(body);
    free(encoded);
    free(result);
    int rc=error?fail(out,error):operation_reply(a,operation,out);
    free(operation);
    return rc;
}
static int undo_plan_create(Actions *a,const Json *request,FILE *out){
    char *source_plan=json_string(request,json_get(request,0,"source_plan_id")),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*body=NULL,*result=NULL;
    size_t revision=0;
    const char *error=NULL;
    int found=0,transaction=0;
    char plan[65]={
        0
    }
    ;
    if(!hash_identity(source_plan)||!identity(operation,128)||!json_uint(request,json_get(request,0,"plan_revision"),&revision)||revision!=1)error="invalid_request";
    if(!error){
        encoded=normalize_operation("undo_plan_create",operation,source_plan,NULL);
        if(!encoded)error="resource_limit";
        else error=retry(a,operation,encoded,&found);
    }
    if(!error&&found==1)goto done;
    if(!error){
        error=lock_actions(a);
        if(error&&found==2&&!strcmp(error,"busy")){
            error=NULL;
            goto done;
        }
    }
    if(!error)error=recover(a);
    if(!error)error=retry(a,operation,encoded,&found);
    if(!error&&found)goto done;
    if(!error){
        sqlite3_int64 plans=scalar(a,"SELECT count(*) FROM plans"),operations=scalar(a,"SELECT count(*) FROM action_operations");
        if(plans<0||operations<0)error="store_failed";
        else if(plans>=32||operations>=128)error="action_limit";
    }
    if(!error)error=root_for_plan(a,source_plan,1);
    sqlite3_stmt *stmt=NULL;
    sqlite3_int64 root_device=0,root_inode=0;
    int inverse_move=0;
    if(!error){
        if(!prepare(a,&stmt,"SELECT body,root_device,root_inode FROM plans WHERE id=? AND owner=?"))error="store_failed";
        else {
            bind(stmt,1,source_plan);
            bind(stmt,2,a->owner);
            if(sqlite3_step(stmt)!=SQLITE_ROW)error="unknown_plan";
            else {
                const char *old_body=(const char*)sqlite3_column_text(stmt,0);
                Json j;
                char *action=NULL;
                if(!old_body||!json_parse(&j,old_body,strlen(old_body))||!(action=json_string(&j,json_get(&j,0,"action")))||(strcmp(action,"copy")&&strcmp(action,"move")))error="unsupported_inverse";
                else inverse_move=!strcmp(action,"move");
                free(action);
                root_device=sqlite3_column_int64(stmt,1);
                root_inode=sqlite3_column_int64(stmt,2);
            }
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    Item items[8]={
        0
    }
    ;
    size_t count=0;
    if(!error){
        if(!prepare(a,&stmt,"SELECT destination,version,payload_device,payload_inode,parent_device,parent_inode,ordinal,source,source_parent_device,source_parent_inode FROM copy_items WHERE plan=? AND status=? AND reversed=0 ORDER BY ordinal LIMIT 8"))error="store_failed";
        else {
            bind(stmt,1,source_plan);
            bind(stmt,2,inverse_move?"moved":"copied");
            for(;;){
                int rc=sqlite3_step(stmt);
                if(rc==SQLITE_DONE)break;
                if(rc!=SQLITE_ROW||count>=8){
                    error="store_failed";
                    break;
                }
                Item *item=&items[count++];
                item->source=column(stmt,0);
                item->destination=inverse_move?column(stmt,7):item->source?strdup(item->source):NULL;
                item->version=column(stmt,1);
                item->device=sqlite3_column_int64(stmt,2);
                item->inode=sqlite3_column_int64(stmt,3);
                item->parent_device=sqlite3_column_int64(stmt,4);
                item->parent_inode=sqlite3_column_int64(stmt,5);
                if(inverse_move){
                    item->source_device=item->device;
                    item->source_inode=item->inode;
                    item->source_parent_device=item->parent_device;
                    item->source_parent_inode=item->parent_inode;
                    item->parent_device=sqlite3_column_int64(stmt,8);
                    item->parent_inode=sqlite3_column_int64(stmt,9);
                }
                if(!item->source||!item->destination||!item->version){
                    error="resource_limit";
                    break;
                }
                const char *anchor_error=anchor_verify(a,source_plan,(size_t)sqlite3_column_int64(stmt,6),item);
                if(anchor_error){
                    error=anchor_error;
                    break;
                }
                if(!item->source||!item->destination||!item->version){
                    error="resource_limit";
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
        if(!error&&!count)error=inverse_move?"no_move_receipts":"no_copy_receipts";
    }
    for(size_t i=0;!error&&i<count;i++){
        Item *item=&items[i];
        char *leaf=NULL;
        int directory=parent(a,item->source,&leaf),fd=directory<0?-1:openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        struct stat info,container;
        if(fd<0||fstat(fd,&info)||fstat(directory,&container)||!S_ISREG(info.st_mode)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode||(sqlite3_int64)container.st_dev!=(inverse_move?item->source_parent_device:item->parent_device)||(sqlite3_int64)container.st_ino!=(inverse_move?item->source_parent_inode:item->parent_inode))error="destination_changed";
        else {
            error=copy_bytes(fd,-1,item->version,NULL,&info);
            if(!error){
                char id[65];
                metadata_identity(&info,id);
                item->identity=strdup(id);
                if(!item->identity)error="resource_limit";
            }
        }
        if(!error&&inverse_move){
            char *destination_leaf=NULL;
            int destination=parent(a,item->destination,&destination_leaf);
            struct stat scope,target;
            if(destination<0||fstat(destination,&scope)||(sqlite3_int64)scope.st_dev!=item->parent_device||(sqlite3_int64)scope.st_ino!=item->parent_inode)error="stale_destination_scope";
            else if(!fstatat(destination,destination_leaf,&target,AT_SYMLINK_NOFOLLOW))error="destination_exists";
            else if(errno!=ENOENT)error="destination_unavailable";
            if(destination>=0)close(destination);
            free(destination_leaf);
        }
        if(fd>=0)close(fd);
        if(directory>=0)close(directory);
        free(leaf);
    }
    if(!error){
        size_t n;
        FILE *stream=open_memstream(&body,&n);
        if(!stream)error="resource_limit";
        else {
            fputs("{\"action\":",stream);
            json_quote(stream,inverse_move?"undo_move":"undo_copy",9);
            fputs(",\"host_id\":",stream);
            json_quote(stream,a->host,strlen(a->host));
            fprintf(stream,",\"source_plan_id\":\"%s\",\"files\":[",source_plan);
            for(size_t i=0;i<count;i++){
                if(i)fputc(',',stream);
                fputs("{\"source\":",stream);
                json_quote(stream,items[i].source,strlen(items[i].source));
                if(inverse_move){
                    fputs(",\"destination\":",stream);
                    json_quote(stream,items[i].destination,strlen(items[i].destination));
                }
                fprintf(stream,",\"version\":\"%s\",\"file_identity\":\"%s\",\"published_device\":%lld,\"published_inode\":%lld}",items[i].version,items[i].identity,(long long)items[i].device,(long long)items[i].inode);
            }
            fputs("]}",stream);
            if(fclose(stream))error="resource_limit";
        }
    }
    if(!error){
        Sha256 hash;
        unsigned char sum[32];
        sha256_init(&hash);
        sha256_update(&hash,a->owner,strlen(a->owner)+1);
        sha256_update(&hash,encoded,strlen(encoded));
        sha256_update(&hash,body,strlen(body));
        sha256_final(&hash,sum);
        sha256_hex(sum,plan);
        if(!sql(a,"BEGIN IMMEDIATE"))error="store_busy";
        else transaction=1;
    }
    if(!error){
        if(!prepare(a,&stmt,"INSERT INTO plans(id,owner,body,root_device,root_inode) VALUES(?,?,?,?,?)"))error="store_failed";
        else {
            bind(stmt,1,plan);
            bind(stmt,2,a->owner);
            bind(stmt,3,body);
            sqlite3_bind_int64(stmt,4,root_device);
            sqlite3_bind_int64(stmt,5,root_inode);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    for(size_t i=0;!error&&i<count;i++){
        if(!prepare(a,&stmt,"INSERT INTO copy_items(plan,ordinal,source,destination,version,identity,parent_device,parent_inode,payload_device,payload_inode,source_device,source_inode,source_parent_device,source_parent_inode) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))error="store_failed";
        else {
            bind(stmt,1,plan);
            sqlite3_bind_int64(stmt,2,(sqlite3_int64)i);
            bind(stmt,3,items[i].source);
            bind(stmt,4,items[i].destination);
            bind(stmt,5,items[i].version);
            bind(stmt,6,items[i].identity);
            sqlite3_bind_int64(stmt,7,items[i].parent_device);
            sqlite3_bind_int64(stmt,8,items[i].parent_inode);
            sqlite3_bind_int64(stmt,9,items[i].device);
            sqlite3_bind_int64(stmt,10,items[i].inode);
            sqlite3_bind_int64(stmt,11,items[i].source_device);
            sqlite3_bind_int64(stmt,12,items[i].source_inode);
            sqlite3_bind_int64(stmt,13,items[i].source_parent_device);
            sqlite3_bind_int64(stmt,14,items[i].source_parent_inode);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!error)error=admit(a,operation,encoded,plan);
    if(!error){
        result=plan_buffer(a,plan);
        if(!result)error="store_failed";
        else error=finish(a,operation,result);
    }
    if(transaction){
        if(error)sql(a,"ROLLBACK");
        else if(!sql(a,"COMMIT")){
            sql(a,"ROLLBACK");
            error="store_failed";
        }
    }
    for(size_t i=0;i<count;i++)item_free(&items[i]);
    done: free(source_plan);
    free(encoded);
    free(body);
    free(result);
    int rc=error?fail(out,error):operation_reply(a,operation,out);
    free(operation);
    return rc;
}
static const char *stage_nonce(char name[64]){
    unsigned char bytes[16];
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(fd<0)return "resource_limit";
    size_t used=0;
    while(used<sizeof(bytes)){
        ssize_t n=read(fd,bytes+used,sizeof(bytes)-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){
            close(fd);
            return "resource_limit";
        }
        used+=(size_t)n;
    }
    close(fd);
    memcpy(name,".samosa-copy-",13);
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<sizeof(bytes);i++){
        name[13+i*2]=hex[bytes[i]>>4];
        name[14+i*2]=hex[bytes[i]&15];
    }
    name[45]=0;
    return NULL;
}
static const char *record_intent(Actions *a,const char *plan,size_t ordinal,const char *stage){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"UPDATE copy_items SET status='intent',stage=? WHERE plan=? AND ordinal=? AND status='pending'"))return "store_failed";
    bind(stmt,1,stage);
    bind(stmt,2,plan);
    sqlite3_bind_int64(stmt,3,(sqlite3_int64)ordinal);
    int rc=sqlite3_step(stmt);
    int changed=sqlite3_changes(a->db);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE&&changed==1?NULL:"store_failed";
}
static const char *record_prepared(Actions *a,const char *plan,size_t ordinal,const struct stat *stage,const struct stat *payload){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"UPDATE copy_items SET status='prepared',stage_device=?,stage_inode=?,payload_device=?,payload_inode=? WHERE plan=? AND ordinal=? AND status='intent'"))return "store_failed";
    sqlite3_bind_int64(stmt,1,(sqlite3_int64)stage->st_dev);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)stage->st_ino);
    sqlite3_bind_int64(stmt,3,(sqlite3_int64)payload->st_dev);
    sqlite3_bind_int64(stmt,4,(sqlite3_int64)payload->st_ino);
    bind(stmt,5,plan);
    sqlite3_bind_int64(stmt,6,(sqlite3_int64)ordinal);
    int rc=sqlite3_step(stmt);
    int changed=sqlite3_changes(a->db);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE&&changed==1?NULL:"store_failed";
}
static int rename_no_replace(int from,const char *source,int to,const char *destination){
#ifdef __APPLE__
    return renameatx_np(from,source,to,destination,RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2,from,source,to,destination,RENAME_NOREPLACE);
#else
    (void)from;
    (void)source;
    (void)to;
    (void)destination;
    errno=ENOTSUP;
    return -1;
#endif
}
static const char *copy_item(Actions *a,const char *plan,size_t ordinal,Item *item){
    if(!strcmp(item->status,"copied")||!strcmp(item->status,"failed"))return NULL;
    char *leaf=NULL,*source_leaf=NULL;
    int destination=parent(a,item->destination,&leaf),source_parent=-1,source=-1,stage=-1,payload=-1;
    const char *error=NULL;
    struct stat directory_info,stage_info,payload_info,captured,live;
    if(destination<0||fstat(destination,&directory_info)||(sqlite3_int64)directory_info.st_dev!=item->parent_device||(sqlite3_int64)directory_info.st_ino!=item->parent_inode)error="stale_destination_scope";
    if(!error&&!strcmp(item->status,"pending")){
        char nonce[64];
        error=stage_nonce(nonce);
        if(!error)error=record_intent(a,plan,ordinal,nonce);
        if(!error){
            item->stage=strdup(nonce);
            if(!item->stage)error="resource_limit";
        }
        if(!error&&mkdirat(destination,item->stage,0700))error="stage_conflict";
        if(!error){
            stage=openat(destination,item->stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(stage<0||fstat(stage,&stage_info))error="copy_failed";
        }
        if(!error){
            payload=openat(stage,"payload",O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
            source_parent=parent(a,item->source,&source_leaf);
            source=source_parent<0?-1:openat(source_parent,source_leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(payload<0)error="copy_failed";
            else if(source<0)error="source_unavailable";
            else error=copy_bytes(source,payload,item->version,item->identity,&captured);
        }
        if(!error&&(fstatat(source_parent,source_leaf,&live,AT_SYMLINK_NOFOLLOW)||!stable(&captured,&live)))error="stale_source";
        if(!error&&(fsync(payload)||fstat(payload,&payload_info)||fsync(stage)))error="copy_failed";
        if(!error){
            error=anchor_create(a,plan,ordinal,stage);
            if(!error)error=record_prepared(a,plan,ordinal,&stage_info,&payload_info);
            if(!error){
                item->device=(sqlite3_int64)payload_info.st_dev;
                item->inode=(sqlite3_int64)payload_info.st_ino;
                item->stage_device=(sqlite3_int64)stage_info.st_dev;
                item->stage_inode=(sqlite3_int64)stage_info.st_ino;
            }
        }
    }
    else if(!error&&!strcmp(item->status,"prepared")){
        if(!item->stage)error="corrupt_plan";
        else {
            stage=openat(destination,item->stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(stage<0||fstat(stage,&stage_info)||(sqlite3_int64)stage_info.st_dev!=item->stage_device||(sqlite3_int64)stage_info.st_ino!=item->stage_inode||stage_info.st_uid!=geteuid()||(stage_info.st_mode&077))error="stage_changed";
        }
        if(!error){
            payload=openat(stage,"payload",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(payload<0||fstat(payload,&payload_info)||(sqlite3_int64)payload_info.st_dev!=item->device||(sqlite3_int64)payload_info.st_ino!=item->inode)error="stage_changed";
            else error=copy_bytes(payload,-1,item->version,NULL,NULL);
        }
    }
    else if(!error)error="needs_reconciliation";
    if(!error)error=anchor_verify(a,plan,ordinal,item);
    if(!error){
        char *checked_leaf=NULL;
        int checked_parent=parent(a,item->destination,&checked_leaf);
        struct stat checked;
        if(checked_parent<0||fstat(checked_parent,&checked)||checked.st_dev!=directory_info.st_dev||checked.st_ino!=directory_info.st_ino)error="stale_destination_scope";
        if(checked_parent>=0)close(checked_parent);
        free(checked_leaf);
    }
    if(!error&&linkat(stage,"payload",destination,leaf,0))error=errno==EEXIST?"destination_exists":"copy_failed";
    int published=!error;
    if(!error&&fsync(destination))error="receipt_pending";
#ifdef SAMOSA_AGENT_TEST_FAULTS
    const char *marker=getenv("SAMOSA_ACTION_TEST_PUBLISHED_MARKER");
    if(published&&marker){
        FILE *ready=fopen(marker,"w");
        if(!ready)_exit(98);
        fputs("published",ready);
        fclose(ready);
        for(;;)pause();
    }
#endif
    if(!error)error=item_status(a,plan,ordinal,"copied",NULL);
    else if(!published){
        const char *stored=item_status(a,plan,ordinal,"failed",error);
        if(stored)error=stored;
    }
    /* Cleanup only known owner staging after its receipt is durable. */ if(!error&&stage>=0)cleanup_stage(destination,item);
    if(source>=0)close(source);
    if(source_parent>=0)close(source_parent);
    if(payload>=0)close(payload);
    if(stage>=0)close(stage);
    if(destination>=0)close(destination);
    free(leaf);
    free(source_leaf);
    if(error&&!published&&strcmp(error,"store_failed"))return NULL;
    return error;
}
static const char *undo_commit(Actions *a,const char *plan,size_t ordinal,const Item *item){
    sqlite3_stmt *stmt=NULL;
    char *source_plan=NULL;
    const char *error=NULL;
    if(!prepare(a,&stmt,"SELECT body FROM plans WHERE id=? AND owner=?"))error="store_failed";
    else {
        bind(stmt,1,plan);
        bind(stmt,2,a->owner);
        if(sqlite3_step(stmt)!=SQLITE_ROW)error="unknown_plan";
        else {
            const char *body=(const char*)sqlite3_column_text(stmt,0);
            Json j;
            if(!body||!json_parse(&j,body,strlen(body))||!(source_plan=json_string(&j,json_get(&j,0,"source_plan_id")))||!hash_identity(source_plan))error="corrupt_plan";
        }
    }
    sqlite3_finalize(stmt);
    stmt=NULL;
    if(!error&&!sql(a,"BEGIN IMMEDIATE"))error="store_busy";
    int transaction=!error;
    if(!error)error=item_status(a,plan,ordinal,"undone",NULL);
    if(!error){
        if(!prepare(a,&stmt,"UPDATE copy_items SET reversed=1 WHERE plan=? AND destination=? AND payload_device=? AND payload_inode=? AND status='copied'"))error="store_failed";
        else {
            bind(stmt,1,source_plan);
            bind(stmt,2,item->source);
            sqlite3_bind_int64(stmt,3,item->device);
            sqlite3_bind_int64(stmt,4,item->inode);
            if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(a->db)!=1)error="corrupt_plan";
        }
        sqlite3_finalize(stmt);
    }
    if(transaction){
        if(error)sql(a,"ROLLBACK");
        else if(!sql(a,"COMMIT")){
            sql(a,"ROLLBACK");
            error="store_failed";
        }
    }
    free(source_plan);
    return error;
}
static const char *undo_ready(Actions *a,const char *plan,size_t ordinal,const struct stat *stage){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"UPDATE copy_items SET status='undo_prepared',stage_device=?,stage_inode=? WHERE plan=? AND ordinal=? AND status='intent'"))return "store_failed";
    sqlite3_bind_int64(stmt,1,(sqlite3_int64)stage->st_dev);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)stage->st_ino);
    bind(stmt,3,plan);
    sqlite3_bind_int64(stmt,4,(sqlite3_int64)ordinal);
    int rc=sqlite3_step(stmt),changed=sqlite3_changes(a->db);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE&&changed==1?NULL:"store_failed";
}
static int undo_stage(int directory,const Item *item,struct stat *info){
    if(!item->stage)return -1;
    int stage=openat(directory,item->stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(stage<0)return -1;
    if(fstat(stage,info)||info->st_uid!=geteuid()||(info->st_mode&077)||(sqlite3_int64)info->st_dev!=item->stage_device||(sqlite3_int64)info->st_ino!=item->stage_inode){
        close(stage);
        return -1;
    }
    return stage;
}
static const char *undo_payload(int stage,const Item *item,int *payload){
    *payload=openat(stage,"payload",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(*payload<0)return errno==ENOENT?"payload_absent":"needs_reconciliation";
    struct stat info;
    if(fstat(*payload,&info)||!S_ISREG(info.st_mode)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)return "needs_reconciliation";
    return copy_bytes(*payload,-1,item->version,item->identity,NULL);
}
static void cleanup_empty_stage(int directory,int stage,const Item *item){
    struct stat held,named;
    if(!fstat(stage,&held)&&!fstatat(directory,item->stage,&named,AT_SYMLINK_NOFOLLOW)&&held.st_dev==named.st_dev&&held.st_ino==named.st_ino)(void)unlinkat(directory,item->stage,AT_REMOVEDIR);
    (void)fsync(directory);
}
static const char *undo_reconcile(Actions *a,const char *plan,size_t ordinal,const Item *item){
    char *leaf=NULL;
    int directory=parent(a,item->destination,&leaf);
    struct stat info;
    const char *error=NULL;
    int stage=-1,payload=-1;
    if(directory<0||fstat(directory,&info)||(sqlite3_int64)info.st_dev!=item->parent_device||(sqlite3_int64)info.st_ino!=item->parent_inode)error="destination_changed";
    if(!error){
        stage=undo_stage(directory,item,&info);
        if(stage<0)error="needs_reconciliation";
    }
    if(!error){
        error=undo_payload(stage,item,&payload);
        if(error&&!strcmp(error,"payload_absent")){
            if(!strcmp(item->status,"undo_removing")){
                error=undo_commit(a,plan,ordinal,item);
                if(!error)cleanup_empty_stage(directory,stage,item);
            }
            else if(!strcmp(item->status,"undo_prepared")){
                int current=openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
                if(current<0||fstat(current,&info)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)error="destination_changed";
                else error=NULL;
                if(current>=0)close(current);
            }
            else error="needs_reconciliation";
        }
        else if(!error&&strcmp(item->status,"undo_removing"))error=item_status(a,plan,ordinal,"undo_captured",NULL);
    }
    if(payload>=0)close(payload);
    if(stage>=0)close(stage);
    if(directory>=0)close(directory);
    free(leaf);
    return error;
}
static const char *undo_item(Actions *a,const char *plan,size_t ordinal,Item *item){
    if(!strcmp(item->status,"undone")||!strcmp(item->status,"failed"))return NULL;
    char *leaf=NULL;
    int directory=parent(a,item->destination,&leaf),stage=-1,payload=-1,current=-1;
    struct stat info,container;
    const char *error=NULL;
    int captured=0;
    if(directory<0||fstat(directory,&container)||(sqlite3_int64)container.st_dev!=item->parent_device||(sqlite3_int64)container.st_ino!=item->parent_inode)error="destination_changed";
    if(!error&&!strcmp(item->status,"pending")){
        current=openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(current<0||fstat(current,&info)||!S_ISREG(info.st_mode)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)error="destination_changed";
        else error=copy_bytes(current,-1,item->version,item->identity,NULL);
        char nonce[64];
        if(!error)error=stage_nonce(nonce);
        if(!error)error=record_intent(a,plan,ordinal,nonce);
        if(!error){
            item->stage=strdup(nonce);
            if(!item->stage)error="resource_limit";
        }
        if(!error&&mkdirat(directory,item->stage,0700))error="stage_conflict";
        if(!error){
            stage=openat(directory,item->stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(stage<0||fstat(stage,&info))error="copy_failed";
            else {
                item->stage_device=(sqlite3_int64)info.st_dev;
                item->stage_inode=(sqlite3_int64)info.st_ino;
                error=undo_ready(a,plan,ordinal,&info);
            }
        }
    }
    else if(!error){
        stage=undo_stage(directory,item,&info);
        if(stage<0)error="needs_reconciliation";
    }
    int needs_capture=!strcmp(item->status,"pending")||!strcmp(item->status,"undo_prepared");
    if(!error&&needs_capture){
        if(fstatat(directory,leaf,&info,AT_SYMLINK_NOFOLLOW)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)error="destination_changed";
#ifdef SAMOSA_AGENT_TEST_FAULTS
        const char *before=getenv("SAMOSA_UNDO_TEST_BEFORE_CAPTURE_MARKER"),*release=getenv("SAMOSA_UNDO_TEST_RELEASE");
        if(!error&&before&&release){
            FILE *ready=fopen(before,"w");
            if(!ready)_exit(98);
            fputs("ready",ready);
            fclose(ready);
            int released=0;
            for(int i=0;i<500;i++){
                if(!access(release,F_OK)){
                    released=1;
                    break;
                }
                struct timespec delay={
                    0,10000000
                }
                ;
                nanosleep(&delay,NULL);
            }
            if(!released)_exit(99);
        }
#endif
        if(!error&&rename_no_replace(directory,leaf,stage,"payload"))error=errno==EEXIST?"needs_reconciliation":"undo_failed";
        if(!error){
            captured=1;
            error=undo_payload(stage,item,&payload);
            if(error){
                if(!rename_no_replace(stage,"payload",directory,leaf)){
                    captured=0;
                    error="destination_changed";
                }
                else error="needs_reconciliation";
            }
            else {
                error=item_status(a,plan,ordinal,"undo_captured",NULL);
                if(!error&&(fsync(stage)||fsync(directory)))error="receipt_pending";
            }
        }
    }
    else if(!error){
        error=undo_payload(stage,item,&payload);
        if(!error)captured=1;
    }
#ifdef SAMOSA_AGENT_TEST_FAULTS
    const char *marker=getenv("SAMOSA_UNDO_TEST_CAPTURED_MARKER");
    if(!error&&captured&&marker){
        FILE *ready=fopen(marker,"w");
        if(!ready)_exit(98);
        fputs("captured",ready);
        fclose(ready);
        for(;;)pause();
    }
#endif
    if(!error)error=item_status(a,plan,ordinal,"undo_removing",NULL);
    if(!error){
        if(lseek(payload,0,SEEK_SET)<0)error="undo_failed";
        else error=copy_bytes(payload,-1,item->version,item->identity,NULL);
    }
    if(!error&&unlinkat(stage,"payload",0))error="undo_failed";
    if(!error&&fsync(stage))error="receipt_pending";
#ifdef SAMOSA_AGENT_TEST_FAULTS
    const char *removed=getenv("SAMOSA_UNDO_TEST_REMOVED_MARKER");
    if(!error&&removed){
        FILE *ready=fopen(removed,"w");
        if(!ready)_exit(98);
        fputs("removed",ready);
        fclose(ready);
        for(;;)pause();
    }
#endif
    if(!error)error=undo_commit(a,plan,ordinal,item);
    if(!error)cleanup_empty_stage(directory,stage,item);
    if(error&&!captured&&strcmp(error,"store_failed")){
        const char *stored=item_status(a,plan,ordinal,"failed",error);
        if(stored)error=stored;
        else error=NULL;
    }
    if(current>=0)close(current);
    if(payload>=0)close(payload);
    if(stage>=0)close(stage);
    if(directory>=0)close(directory);
    free(leaf);
    return error;
}
static const char *move_commit(Actions *a,const char *plan,size_t ordinal,const Item *item){
    sqlite3_stmt *stmt=NULL;
    char *source_plan=NULL;
    const char *error=NULL;
    int inverse=0;
    if(!prepare(a,&stmt,"SELECT body FROM plans WHERE id=? AND owner=?"))error="store_failed";
    else {
        bind(stmt,1,plan);
        bind(stmt,2,a->owner);
        if(sqlite3_step(stmt)!=SQLITE_ROW)error="unknown_plan";
        else {
            const char *body=(const char*)sqlite3_column_text(stmt,0);
            Json j;
            char *action=NULL;
            if(!body||!json_parse(&j,body,strlen(body))||!(action=json_string(&j,json_get(&j,0,"action"))))error="corrupt_plan";
            else {
                inverse=!strcmp(action,"undo_move");
                if(inverse){
                    source_plan=json_string(&j,json_get(&j,0,"source_plan_id"));
                    if(!hash_identity(source_plan))error="corrupt_plan";
                }
                else if(strcmp(action,"move"))error="corrupt_plan";
            }
            free(action);
        }
    }
    sqlite3_finalize(stmt);
    stmt=NULL;
    if(!error&&!inverse)return item_status(a,plan,ordinal,"moved",NULL);
    if(!error&&!sql(a,"BEGIN IMMEDIATE"))error="store_busy";
    int transaction=!error;
    if(!error)error=item_status(a,plan,ordinal,"undone",NULL);
    if(!error){
        if(!prepare(a,&stmt,"UPDATE copy_items SET reversed=1 WHERE plan=? AND destination=? AND payload_device=? AND payload_inode=? AND status='moved'"))error="store_failed";
        else {
            bind(stmt,1,source_plan);
            bind(stmt,2,item->source);
            sqlite3_bind_int64(stmt,3,item->device);
            sqlite3_bind_int64(stmt,4,item->inode);
            if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(a->db)!=1)error="corrupt_plan";
        }
        sqlite3_finalize(stmt);
    }
    if(transaction){
        if(error)sql(a,"ROLLBACK");
        else if(!sql(a,"COMMIT")){
            sql(a,"ROLLBACK");
            error="store_failed";
        }
    }
    free(source_plan);
    return error;
}
static const char *move_ready(Actions *a,const char *plan,size_t ordinal,const Item *item,const struct stat *stage){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(a,&stmt,"UPDATE copy_items SET status='move_prepared',stage_device=?,stage_inode=?,payload_device=?,payload_inode=? WHERE plan=? AND ordinal=? AND status='intent'"))return "store_failed";
    sqlite3_bind_int64(stmt,1,(sqlite3_int64)stage->st_dev);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)stage->st_ino);
    sqlite3_bind_int64(stmt,3,item->source_device);
    sqlite3_bind_int64(stmt,4,item->source_inode);
    bind(stmt,5,plan);
    sqlite3_bind_int64(stmt,6,(sqlite3_int64)ordinal);
    int rc=sqlite3_step(stmt),changed=sqlite3_changes(a->db);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE&&changed==1?NULL:"store_failed";
}
static const char *move_reconcile(Actions *a,const char *plan,size_t ordinal,const Item *item){
    char *leaf=NULL;
    int directory=parent(a,item->destination,&leaf),stage=-1,payload=-1,target=-1;
    struct stat info;
    const char *error=NULL;
    if(directory<0||fstat(directory,&info)||(sqlite3_int64)info.st_dev!=item->parent_device||(sqlite3_int64)info.st_ino!=item->parent_inode)error="needs_reconciliation";
    if(!error){
        target=openat(directory,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(target>=0&&!fstat(target,&info)&&S_ISREG(info.st_mode)&&(sqlite3_int64)info.st_dev==item->device&&(sqlite3_int64)info.st_ino==item->inode){
            error=copy_bytes(target,-1,item->version,NULL,NULL);
            if(!error)error=anchor_verify(a,plan,ordinal,item);
            if(!error)error=move_commit(a,plan,ordinal,item);
            if(!error){
                stage=undo_stage(directory,item,&info);
                if(stage>=0)cleanup_empty_stage(directory,stage,item);
            }
            goto done;
        }
        stage=undo_stage(directory,item,&info);
        if(stage<0)error="needs_reconciliation";
    }
    if(!error){
        error=undo_payload(stage,item,&payload);
        if(error&&!strcmp(error,"payload_absent")&&!strcmp(item->status,"move_prepared")){
            char *source_leaf=NULL;
            int source_parent=parent(a,item->source,&source_leaf);
            int source=source_parent<0?-1:openat(source_parent,source_leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(source<0||fstat(source,&info)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode)error="needs_reconciliation";
            else error=NULL;
            if(source>=0)close(source);
            if(source_parent>=0)close(source_parent);
            free(source_leaf);
        }
        else if(!error)error=item_status(a,plan,ordinal,"move_captured",target>=0?"destination_exists":NULL);
        else if(!strcmp(error,"payload_absent"))error="needs_reconciliation";
    }
    done: if(target>=0)close(target);
    if(payload>=0)close(payload);
    if(stage>=0)close(stage);
    if(directory>=0)close(directory);
    free(leaf);
    return error;
}
static const char *move_item(Actions *a,const char *plan,size_t ordinal,Item *item){
    if(!strcmp(item->status,"moved")||!strcmp(item->status,"undone")||!strcmp(item->status,"failed"))return NULL;
    char *leaf=NULL,*source_leaf=NULL;
    int destination=parent(a,item->destination,&leaf),source_parent=-1,source=-1,stage=-1,payload=-1;
    struct stat container,info;
    const char *error=NULL;
    int captured=0,published=0;
    if(destination<0||fstat(destination,&container)||(sqlite3_int64)container.st_dev!=item->parent_device||(sqlite3_int64)container.st_ino!=item->parent_inode)error="stale_destination_scope";
    if(!error&&!strcmp(item->status,"pending")){
        source_parent=parent(a,item->source,&source_leaf);
        source=source_parent<0?-1:openat(source_parent,source_leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(source<0||fstat(source_parent,&info)||(sqlite3_int64)info.st_dev!=item->source_parent_device||(sqlite3_int64)info.st_ino!=item->source_parent_inode)error="stale_source_scope";
        else if(fstat(source,&info)||(sqlite3_int64)info.st_dev!=item->source_device||(sqlite3_int64)info.st_ino!=item->source_inode)error="stale_source";
        else error=copy_bytes(source,-1,item->version,item->identity,NULL);
        if(!error){
            int anchors=anchors_open(a);
            if(anchors<0)error="state_unavailable";
            else {
                if(fstat(anchors,&info)||info.st_dev!=container.st_dev||info.st_dev!=(dev_t)item->source_device)error="cross_device_move";
                close(anchors);
            }
        }
        if(!error){
            if(!fstatat(destination,leaf,&info,AT_SYMLINK_NOFOLLOW))error="destination_exists";
            else if(errno!=ENOENT)error="destination_unavailable";
        }
        char nonce[64];
        if(!error)error=stage_nonce(nonce);
        if(!error)error=record_intent(a,plan,ordinal,nonce);
        if(!error){
            item->stage=strdup(nonce);
            if(!item->stage)error="resource_limit";
        }
        if(!error&&mkdirat(destination,item->stage,0700))error="stage_conflict";
        if(!error){
            stage=openat(destination,item->stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(stage<0||fstat(stage,&info))error="move_failed";
            else {
                item->stage_device=(sqlite3_int64)info.st_dev;
                item->stage_inode=(sqlite3_int64)info.st_ino;
                item->device=item->source_device;
                item->inode=item->source_inode;
                error=move_ready(a,plan,ordinal,item,&info);
            }
        }
    }
    else if(!error){
        stage=undo_stage(destination,item,&info);
        if(stage<0)error="needs_reconciliation";
    }
    int needs_capture=!strcmp(item->status,"pending")||!strcmp(item->status,"move_prepared");
    if(!error&&needs_capture){
        if(source_parent<0)source_parent=parent(a,item->source,&source_leaf);
        if(source_parent<0||fstat(source_parent,&info)||(sqlite3_int64)info.st_dev!=item->source_parent_device||(sqlite3_int64)info.st_ino!=item->source_parent_inode)error="stale_source_scope";
        if(!error&&(fstatat(source_parent,source_leaf,&info,AT_SYMLINK_NOFOLLOW)||(sqlite3_int64)info.st_dev!=item->device||(sqlite3_int64)info.st_ino!=item->inode))error="stale_source";
#ifdef SAMOSA_AGENT_TEST_FAULTS
        const char *before=getenv("SAMOSA_MOVE_TEST_BEFORE_CAPTURE_MARKER"),*release=getenv("SAMOSA_MOVE_TEST_RELEASE");
        if(!error&&before&&release){
            FILE *ready=fopen(before,"w");
            if(!ready)_exit(98);
            fputs("ready",ready);
            fclose(ready);
            int released=0;
            for(int i=0;i<500;i++){
                if(!access(release,F_OK)){
                    released=1;
                    break;
                }
                struct timespec delay={
                    0,10000000
                }
                ;
                nanosleep(&delay,NULL);
            }
            if(!released)_exit(99);
        }
#endif
        if(!error&&rename_no_replace(source_parent,source_leaf,stage,"payload"))error="move_failed";
        if(!error){
            captured=1;
            error=undo_payload(stage,item,&payload);
            if(error){
                if(!rename_no_replace(stage,"payload",source_parent,source_leaf)){
                    captured=0;
                    error="stale_source";
                }
                else error="needs_reconciliation";
            }
            else {
                error=item_status(a,plan,ordinal,"move_captured",NULL);
                if(!error&&(fsync(stage)||fsync(source_parent)||fsync(destination)))error="receipt_pending";
            }
        }
    }
    else if(!error){
        error=undo_payload(stage,item,&payload);
        if(!error)captured=1;
    }
#ifdef SAMOSA_AGENT_TEST_FAULTS
    const char *capture_marker=getenv("SAMOSA_MOVE_TEST_CAPTURED_MARKER");
    if(!error&&captured&&capture_marker){
        FILE *ready=fopen(capture_marker,"w");
        if(!ready)_exit(98);
        fputs("captured",ready);
        fclose(ready);
        for(;;)pause();
    }
#endif
    if(!error){
        char name[80];
        anchor_name(plan,ordinal,name);
        int anchors=anchors_open(a);
        if(anchors<0)error="state_unavailable";
        else {
            if(fstatat(anchors,name,&info,AT_SYMLINK_NOFOLLOW)){
                if(errno==ENOENT)error=anchor_create(a,plan,ordinal,stage);
                else error="state_unavailable";
            }
            close(anchors);
        }
    }
    if(!error)error=anchor_verify(a,plan,ordinal,item);
    if(!error){
        char *checked_leaf=NULL;
        int checked=parent(a,item->destination,&checked_leaf);
        if(checked<0||fstat(checked,&info)||info.st_dev!=container.st_dev||info.st_ino!=container.st_ino)error="stale_destination_scope";
        if(checked>=0)close(checked);
        free(checked_leaf);
    }
    if(!error&&rename_no_replace(stage,"payload",destination,leaf))error=errno==EEXIST?"destination_exists":"move_failed";
    if(!error){
        published=1;
        if(fsync(destination)||fsync(stage))error="receipt_pending";
    }
#ifdef SAMOSA_AGENT_TEST_FAULTS
    const char *publish_marker=getenv("SAMOSA_MOVE_TEST_PUBLISHED_MARKER");
    if(published&&publish_marker){
        FILE *ready=fopen(publish_marker,"w");
        if(!ready)_exit(98);
        fputs("published",ready);
        fclose(ready);
        for(;;)pause();
    }
#endif
    if(!error)error=move_commit(a,plan,ordinal,item);
    if(!error)cleanup_empty_stage(destination,stage,item);
    if(error&&!captured&&!published&&strcmp(error,"store_failed")){
        const char *stored=item_status(a,plan,ordinal,"failed",error);
        error=stored;
    }
    if(source>=0)close(source);
    if(source_parent>=0)close(source_parent);
    if(payload>=0)close(payload);
    if(stage>=0)close(stage);
    if(destination>=0)close(destination);
    free(leaf);
    free(source_leaf);
    return error;
}
static int approve_or_execute(Actions *a,const Json *request,int execute,FILE *out){
    char *plan=json_string(request,json_get(request,0,"plan_id")),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*result=NULL;
    size_t revision=0;
    const char *error=NULL;
    int found=0,transaction=0;
    if(!hash_identity(plan)||!identity(operation,128)||!json_uint(request,json_get(request,0,"plan_revision"),&revision)||revision!=1)error="invalid_request";
    if(!error){
        encoded=normalize_operation(execute?"plan_execute":"plan_approve",operation,plan,NULL);
        if(!encoded)error="resource_limit";
        else error=retry(a,operation,encoded,&found);
    }
    if(!error&&found==1)goto done;
    if(!error){
        error=lock_actions(a);
        if(error&&found==2&&!strcmp(error,"busy")){
            error=NULL;
            goto done;
        }
    }
    if(!error)error=recover(a);
    if(!error)error=retry(a,operation,encoded,&found);
    if(!error&&found)goto done;
    sqlite3_stmt *stmt=NULL;
    if(!error){
        if(!prepare(a,&stmt,"SELECT approved FROM plans WHERE id=? AND owner=?"))error="store_failed";
        else {
            bind(stmt,1,plan);
            bind(stmt,2,a->owner);
            int rc=sqlite3_step(stmt);
            if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_plan":"store_failed";
            else if(execute&&!sqlite3_column_int(stmt,0))error="approval_required";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!error&&execute)error=root_for_plan(a,plan,1);
    if(!error){
        if(!sql(a,"BEGIN IMMEDIATE"))error="store_busy";
        else transaction=1;
    }
    if(!error)error=admit(a,operation,encoded,plan);
    if(!error&&!execute){
        if(!prepare(a,&stmt,"UPDATE plans SET approved=1 WHERE id=? AND owner=?"))error="store_failed";
        else {
            bind(stmt,1,plan);
            bind(stmt,2,a->owner);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(transaction){
        if(error)sql(a,"ROLLBACK");
        else if(!sql(a,"COMMIT")){
            sql(a,"ROLLBACK");
            error="store_failed";
        }
        transaction=0;
    }
    int undo=0,move=0;
    if(!error&&execute){
        if(!prepare(a,&stmt,"SELECT body FROM plans WHERE id=? AND owner=?"))error="store_failed";
        else {
            bind(stmt,1,plan);
            bind(stmt,2,a->owner);
            if(sqlite3_step(stmt)!=SQLITE_ROW)error="unknown_plan";
            else {
                const char *body=(const char*)sqlite3_column_text(stmt,0);
                Json parsed;
                char *action=NULL;
                if(!body||!json_parse(&parsed,body,strlen(body))||!(action=json_string(&parsed,json_get(&parsed,0,"action"))))error="corrupt_plan";
                else if(!strcmp(action,"undo_copy"))undo=1;
                else if(!strcmp(action,"move")||!strcmp(action,"undo_move"))move=1;
                else if(strcmp(action,"copy"))error="unsupported_action";
                free(action);
            }
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!error&&execute){
        for(size_t ordinal=0;ordinal<8;ordinal++){
            if(!prepare(a,&stmt,"SELECT count(*) FROM copy_items WHERE plan=? AND ordinal=?")){
                error="store_failed";
                break;
            }
            bind(stmt,1,plan);
            sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);
            int rc=sqlite3_step(stmt);
            int exists=rc==SQLITE_ROW&&sqlite3_column_int(stmt,0)>0;
            sqlite3_finalize(stmt);
            stmt=NULL;
            if(rc!=SQLITE_ROW){
                error="store_failed";
                break;
            }
            if(!exists)break;
            Item item={
                0
            }
            ;
            if(!load_item(a,plan,ordinal,&item))error="corrupt_plan";
            else error=move?move_item(a,plan,ordinal,&item):undo?undo_item(a,plan,ordinal,&item):copy_item(a,plan,ordinal,&item);
            item_free(&item);
            if(error)break;
        }
    }
    if(!error){
        result=plan_buffer(a,plan);
        if(!result)error="store_failed";
        if(!error&&execute){
            sqlite3_int64 root_device=0,root_inode=0;
            if(!prepare(a,&stmt,"SELECT root_device,root_inode FROM plans WHERE id=? AND owner=?"))error="store_failed";
            else{
                bind(stmt,1,plan);bind(stmt,2,a->owner);
                if(sqlite3_step(stmt)!=SQLITE_ROW)error="unknown_plan";
                else{root_device=sqlite3_column_int64(stmt,0);root_inode=sqlite3_column_int64(stmt,1);}
            }
            sqlite3_finalize(stmt);stmt=NULL;
            if(!error){
                char *qualified=action_reconcile(a->cfg->sources_command,plan,operation,a->host,a->owner,
                                                (uint64_t)root_device,(uint64_t)root_inode,result);
                if(!qualified)error="resource_limit";
                else{free(result);result=qualified;}
            }
        }
        if(!error)error=finish(a,operation,result);
    }
    done: free(plan);
    free(encoded);
    free(result);
    int rc=error?fail(out,error):operation_reply(a,operation,out);
    free(operation);
    return rc;
}
int agent_actions(const AgentConfig *cfg,const char *input,size_t length,FILE *out){
    if(length>AGENT_REQUEST_MAX)return fail(out,"request_too_large");
    Json j;
    size_t version=0;
    if(!json_parse(&j,input,length)||j.tokens[0].type!=JT_OBJECT||!json_uint(&j,json_get(&j,0,"contract_version"),&version)||version!=1)return fail(out,"invalid_request");
    char *op=json_string(&j,json_get(&j,0,"op"));
    if(!op)return fail(out,"invalid_request");
    int create=!strcmp(op,"plan_create"),inverse=!strcmp(op,"undo_plan_create"),get=!strcmp(op,"plan_get"),approve=!strcmp(op,"plan_approve"),execute=!strcmp(op,"plan_execute");
    const char *fields=create?"|op|contract_version|operation_id|host_id|action|files|":inverse?"|op|contract_version|operation_id|source_plan_id|plan_revision|":get?"|op|contract_version|plan_id|":approve||execute?"|op|contract_version|operation_id|plan_id|plan_revision|":NULL;
    if(!fields||!fields_allowed(&j,0,fields)){
        free(op);
        return fail(out,fields?"unknown_field":"unsupported_operation");
    }
    Actions a;
    const char *error=open_actions(&a,cfg);
    char *result=NULL;
    size_t n=0;
    FILE *stream=NULL;
    int rc=1;
    if(!error){
        stream=open_memstream(&result,&n);
        if(!stream)error="resource_limit";
    }
    if(!error){
        if(create)rc=create_plan(&a,&j,stream);
        else if(inverse)rc=undo_plan_create(&a,&j,stream);
        else if(approve||execute)rc=approve_or_execute(&a,&j,execute,stream);
        else {
            char *plan=json_string(&j,json_get(&j,0,"plan_id"));
            if(!hash_identity(plan))rc=fail(stream,"invalid_request");
            else {
                const char *locked=lock_actions(&a);
                if(!locked)error=recover(&a);
                else if(strcmp(locked,"busy"))error=locked;
                if(!error){
                    if(!sql(&a,"BEGIN"))error="store_busy";
                    else {
                        rc=plan_reply(&a,plan,stream);
                        if(!sql(&a,rc?"ROLLBACK":"COMMIT")){
                            sql(&a,"ROLLBACK");
                            error="store_failed";
                        }
                    }
                }
            }
            free(plan);
        }
        if(fclose(stream))error="resource_limit";
    }
    release(&a);
    free(op);
    if(error){
        free(result);
        return fail(out,error);
    }
    if(n>1048576){
        free(result);
        return fail(out,"response_too_large");
    }
    if(rc){
        Json response;
        if(!result||!json_parse(&response,result,n)||json_get(&response,0,"error")<0){
            free(result);
            return fail(out,"corrupt_plan");
        }
    }
    fwrite(result,1,n,out);
    free(result);
    return rc;
}
