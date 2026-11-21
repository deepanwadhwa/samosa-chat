#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "store.h"
#include "json.h"
#include "sha256.h"
#include "budget.h"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <dirent.h>
#include <unistd.h>

#define RUN_RESULT_MAX 1048576u
#define SEMANTIC_MAX (32u*1048576u)
#define ACTIVE_MAX 8
typedef struct { sqlite3 *db;int directory,lock,worker;const char *owner;char worker_path[80]; } Store;
typedef struct {Store *store;const AgentConfig *upstream;const char *operation;int invalid;} RunControl;
static const char *state_failure(int line) {
#ifdef SAMOSA_AGENT_TEST_FAULTS
    if(getenv("SAMOSA_AGENT_TEST_HELPER_STDERR"))fprintf(stderr,"QA Agent state failure: line=%d errno=%d\n",line,errno);
#else
    (void)line;
#endif
    return "state_unavailable";
}
static int fail(FILE *out,const char *code) {
    fputs("{\"contract_version\":1,\"error\":{\"code\":",out);json_quote(out,code,strlen(code));fputs("}}\n",out);return 1;
}
static int execsql(Store *s,const char *sql){return sqlite3_exec(s->db,sql,NULL,NULL,NULL)==SQLITE_OK;}
static int prepare(Store *s,sqlite3_stmt **stmt,const char *sql){return sqlite3_prepare_v2(s->db,sql,-1,stmt,NULL)==SQLITE_OK;}
static void text(sqlite3_stmt *stmt,int column,const char *value){sqlite3_bind_text(stmt,column,value,-1,SQLITE_TRANSIENT);}
static int identity(const char *value,size_t max) {
    if(!value||!*value||strlen(value)>max)return 0;
    for(const char *p=value;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p))return 0;
    return 1;
}
static int hash_identity(const char *value) {
    if(!value||strlen(value)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!((value[i]>='0'&&value[i]<='9')||(value[i]>='a'&&value[i]<='f')))return 0;
    return 1;
}
static void unlock_store(Store *s){if(s->lock>=0){close(s->lock);s->lock=-1;}}
static void close_store(Store *s){unlock_store(s);if(s->worker>=0)close(s->worker);if(s->db)sqlite3_close(s->db);if(s->directory>=0)close(s->directory);}
static const char *open_store(Store *s,const AgentConfig *cfg) {
    memset(s,0,sizeof(*s));s->directory=s->lock=s->worker=-1;
    if(!cfg||!cfg->state||!identity(cfg->owner,128))return state_failure(__LINE__);
    s->owner=cfg->owner;
    if(mkdir(cfg->state,0700)<0&&errno!=EEXIST)return state_failure(__LINE__);
    s->directory=open(cfg->state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(s->directory<0)return state_failure(__LINE__);
    struct stat st;if(fstat(s->directory,&st)<0||st.st_uid!=geteuid()||(st.st_mode&077))return state_failure(__LINE__);
    int fd=openat(s->directory,"agent.sqlite3",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return state_failure(__LINE__);
    int valid=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&077)&&st.st_nlink==1;close(fd);if(!valid)return state_failure(__LINE__);
    size_t n=strlen(cfg->state)+32;char *path=malloc(n);if(!path)return "resource_limit";snprintf(path,n,"%s/agent.sqlite3",cfg->state);
    int rc=sqlite3_open_v2(path,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);free(path);
    if(rc!=SQLITE_OK){if(rc==SQLITE_BUSY||rc==SQLITE_LOCKED)return "store_busy";return state_failure(__LINE__);}
    sqlite3_busy_timeout(s->db,2000);sqlite3_limit(s->db,SQLITE_LIMIT_LENGTH,2*RUN_RESULT_MAX);
    if(!execsql(s,"PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA max_page_count=16384;"
        "CREATE TABLE IF NOT EXISTS conversations(id TEXT PRIMARY KEY,owner TEXT NOT NULL,revision INTEGER NOT NULL DEFAULT 0,scope TEXT,evidence TEXT,continuation TEXT,summary TEXT,history_from INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS runs(id TEXT NOT NULL UNIQUE,owner TEXT NOT NULL,operation TEXT NOT NULL,conversation TEXT NOT NULL,request TEXT NOT NULL,status TEXT NOT NULL,result TEXT,PRIMARY KEY(owner,operation));"
        "CREATE TABLE IF NOT EXISTS run_controls(owner TEXT NOT NULL,operation TEXT NOT NULL,cancel_requested INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(owner,operation));"
        "CREATE TABLE IF NOT EXISTS run_children(owner TEXT NOT NULL,operation TEXT NOT NULL,child_id TEXT NOT NULL,kind TEXT NOT NULL,status TEXT NOT NULL,cancellation TEXT NOT NULL,ordinal INTEGER NOT NULL,PRIMARY KEY(owner,operation,child_id));"
        "CREATE TABLE IF NOT EXISTS messages(sequence INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL,operation TEXT NOT NULL,role TEXT NOT NULL,content TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS message_conversation ON messages(conversation,sequence);"
        "CREATE TABLE IF NOT EXISTS events(sequence INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL,operation TEXT NOT NULL,status TEXT NOT NULL,revision INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS event_conversation ON events(conversation,sequence);")) {
        int code=sqlite3_errcode(s->db);return code==SQLITE_BUSY||code==SQLITE_LOCKED?"store_busy":"store_failed";
    }
    return NULL;
}
static const char *lock_store(Store *s) {
    if(s->lock>=0)return NULL;
    int fd=openat(s->directory,"work.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return state_failure(__LINE__);
    struct stat st;if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1){close(fd);return state_failure(__LINE__);}
    if(flock(fd,LOCK_EX|LOCK_NB)<0){close(fd);return "busy";}s->lock=fd;return NULL;
}
/* The admission gate bounds creation of these files to the durable run history.
 * A kernel lease, never a PID or pathname timestamp, proves worker liveness. */
static const char *worker_lease(Store *s,const char *id,int *lease) {
    *lease=-1;if(!hash_identity(id))return "store_failed";
    char path[80];snprintf(path,sizeof(path),"run-%s.lock",id);
    int fd=openat(s->directory,path,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return state_failure(__LINE__);
    struct stat st;if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1){close(fd);return state_failure(__LINE__);}
    if(flock(fd,LOCK_EX|LOCK_NB)<0){int blocked=errno==EWOULDBLOCK||errno==EAGAIN;close(fd);return blocked?"busy":"state_unavailable";}
    *lease=fd;return NULL;
}
static int event(Store *s,const char *conversation,const char *operation,const char *status,size_t revision) {
    sqlite3_stmt *stmt=NULL;int ok=prepare(s,&stmt,"INSERT INTO events(conversation,operation,status,revision) VALUES(?,?,?,?)");
    if(ok){text(stmt,1,conversation);text(stmt,2,operation);text(stmt,3,status);sqlite3_bind_int64(stmt,4,(sqlite3_int64)revision);ok=sqlite3_step(stmt)==SQLITE_DONE;}sqlite3_finalize(stmt);return ok;
}
static const char *orphan_leases(Store *s) {
    int fd=openat(s->directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return state_failure(__LINE__);
    DIR *dir=fdopendir(fd);if(!dir){close(fd);return state_failure(__LINE__);}
    const char *error=NULL;struct dirent *entry;unsigned inspected=0;
    for(;;) {
        errno=0;entry=readdir(dir);if(!entry){if(errno)error="state_unavailable";break;}
        if(++inspected>1024){error="storage_limit";break;}
        if(strlen(entry->d_name)!=73||strncmp(entry->d_name,"run-",4)||strcmp(entry->d_name+68,".lock"))continue;
        char id[65];memcpy(id,entry->d_name+4,64);id[64]=0;if(!hash_identity(id)){error="state_unavailable";break;}
        sqlite3_stmt *stmt=NULL;if(!prepare(s,&stmt,"SELECT 1 FROM runs WHERE id=?")){error="store_failed";break;}
        text(stmt,1,id);int rc=sqlite3_step(stmt);sqlite3_finalize(stmt);if(rc==SQLITE_ROW)continue;
        if(rc!=SQLITE_DONE){error="store_failed";break;}
        int lease=-1;error=worker_lease(s,id,&lease);if(error)break;
        struct stat held,named;
        if(fstat(lease,&held)||fstatat(s->directory,entry->d_name,&named,AT_SYMLINK_NOFOLLOW)||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||unlinkat(s->directory,entry->d_name,0))error="state_unavailable";
        close(lease);if(error)break;
    }
    closedir(dir);return error;
}
static const char *recover(Store *s) {
    const char *orphan=orphan_leases(s);if(orphan)return orphan;
    if(!execsql(s,"BEGIN IMMEDIATE"))return "store_busy";
    sqlite3_stmt *stmt=NULL;int leases[256],count=0;const char *failure=NULL;
    int ok=prepare(s,&stmt,"SELECT runs.owner,runs.operation,conversation,coalesce(run_controls.cancel_requested,0),runs.id FROM runs LEFT JOIN run_controls ON runs.owner=run_controls.owner AND runs.operation=run_controls.operation WHERE status='running'");
    int step=SQLITE_DONE;
    while(ok&&(step=sqlite3_step(stmt))==SQLITE_ROW) {
        int lease=-1;const char *locked=worker_lease(s,(const char*)sqlite3_column_text(stmt,4),&lease);
        if(locked){if(!strcmp(locked,"busy"))continue;failure=locked;ok=0;break;}
        if(count==256){close(lease);ok=0;break;}leases[count++]=lease;
        const char *owner=(const char*)sqlite3_column_text(stmt,0),*operation=(const char*)sqlite3_column_text(stmt,1),*conversation=(const char*)sqlite3_column_text(stmt,2);
        int cancelled=sqlite3_column_int(stmt,3)!=0;const char *status=cancelled?"cancelled":"interrupted";
        const char *result=cancelled?"{\"contract_version\":1,\"status\":\"cancelled\",\"error\":{\"code\":\"cancelled\"}}":"{\"contract_version\":1,\"status\":\"interrupted\",\"error\":{\"code\":\"interrupted\"}}";
        sqlite3_stmt *update=NULL;ok=prepare(s,&update,"UPDATE runs SET status=?,result=? WHERE owner=? AND operation=?");
        if(ok){text(update,1,status);text(update,2,result);text(update,3,owner);text(update,4,operation);ok=sqlite3_step(update)==SQLITE_DONE;}sqlite3_finalize(update);
        sqlite3_stmt *rev=NULL;size_t revision=0;
        if(ok){ok=prepare(s,&rev,"SELECT revision FROM conversations WHERE id=?");if(ok){text(rev,1,conversation);ok=sqlite3_step(rev)==SQLITE_ROW;if(ok)revision=(size_t)sqlite3_column_int64(rev,0);}sqlite3_finalize(rev);}
        if(ok)ok=event(s,conversation,operation,status,revision);
        if(ok) {
            sqlite3_stmt *children=NULL;ok=prepare(s,&children,"UPDATE run_children SET status='unconfirmed',cancellation=? WHERE owner=? AND operation=? AND status='running'");
            if(ok){text(children,1,cancelled?"unconfirmed":"not_requested");text(children,2,owner);text(children,3,operation);ok=sqlite3_step(children)==SQLITE_DONE;}sqlite3_finalize(children);
        }
    }
    if(step!=SQLITE_DONE)ok=0;
    sqlite3_finalize(stmt);if(!ok||!execsql(s,"COMMIT")){execsql(s,"ROLLBACK");failure=failure?failure:"store_failed";}
    for(int i=0;i<count;i++)close(leases[i]);
    return failure;
}
static const char *conversation_idle(Store *s,const char *id) {
    sqlite3_stmt *stmt=NULL;if(!prepare(s,&stmt,"SELECT 1 FROM runs WHERE conversation=? AND status='running' LIMIT 1"))return "store_failed";
    text(stmt,1,id);int rc=sqlite3_step(stmt);sqlite3_finalize(stmt);
    return rc==SQLITE_ROW?"busy":rc==SQLITE_DONE?NULL:"store_failed";
}
static sqlite3_int64 scalar(Store *s,const char *sql) {
    sqlite3_stmt *stmt=NULL;sqlite3_int64 value=-1;
    if(prepare(s,&stmt,sql)&&sqlite3_step(stmt)==SQLITE_ROW)value=sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt);return value;
}
static sqlite3_int64 usage(Store *s) {
    return scalar(s,"SELECT (SELECT coalesce(sum(coalesce(length(CAST(scope AS BLOB)),0)+coalesce(length(CAST(evidence AS BLOB)),0)+coalesce(length(CAST(continuation AS BLOB)),0)+coalesce(length(CAST(summary AS BLOB)),0)),0) FROM conversations)"
        "+(SELECT coalesce(sum(length(CAST(request AS BLOB))+coalesce(length(CAST(result AS BLOB)),0)),0) FROM runs)"
        "+(SELECT coalesce(sum(length(CAST(content AS BLOB))),0) FROM messages)");
}
static char *random_id(void) {
    unsigned char random[24];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)return NULL;size_t n=0;
    while(n<sizeof(random)){ssize_t got=read(fd,random+n,sizeof(random)-n);if(got<0&&errno==EINTR)continue;if(got<=0){close(fd);return NULL;}n+=(size_t)got;}close(fd);
    char *id=malloc(49);if(!id)return NULL;const char *hex="0123456789abcdef";
    for(size_t i=0;i<sizeof(random);i++){id[i*2]=hex[random[i]>>4];id[i*2+1]=hex[random[i]&15];}id[48]=0;return id;
}
static int create(Store *s,FILE *out) {
    const char *error=NULL;char *id=random_id();if(!id)error="resource_limit";
    int transaction=0;
    if(!error){if(!execsql(s,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    if(!error){sqlite3_int64 count=scalar(s,"SELECT count(*) FROM conversations");if(count<0)error="store_failed";else if(count>=64)error="storage_limit";}
    sqlite3_stmt *stmt=NULL;
    if(!error){if(!prepare(s,&stmt,"INSERT INTO conversations(id,owner) VALUES(?,?)"))error="store_failed";else{text(stmt,1,id);text(stmt,2,s->owner);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);}
    if(transaction){if(error)execsql(s,"ROLLBACK");else if(!execsql(s,"COMMIT")){execsql(s,"ROLLBACK");error="store_failed";}}
    if(!error)fprintf(out,"{\"contract_version\":1,\"conversation_id\":\"%s\",\"revision\":0}\n",id);
    free(id);return error?fail(out,error):0;
}
static void raw(FILE *out,const Json *j,int token){fwrite(j->text+j->tokens[token].begin,1,j->tokens[token].end-j->tokens[token].begin,out);}
static char *copy_column(sqlite3_stmt *stmt,int column) {
    const char *value=(const char*)sqlite3_column_text(stmt,column);return value?strdup(value):NULL;
}
static int get(Store *s,const char *id,FILE *out) {
    sqlite3_stmt *stmt=NULL;
    if(!prepare(s,&stmt,"SELECT revision,scope,evidence,continuation,summary FROM conversations WHERE id=? AND owner=?"))return fail(out,"store_failed");
    text(stmt,1,id);text(stmt,2,s->owner);int rc=sqlite3_step(stmt);
    if(rc!=SQLITE_ROW){sqlite3_finalize(stmt);return fail(out,rc==SQLITE_DONE?"unknown_conversation":"store_failed");}
    fprintf(out,"{\"contract_version\":1,\"conversation_id\":\"%s\",\"revision\":%lld,\"selected_scope\":%s,\"admitted_evidence\":%s",id,(long long)sqlite3_column_int64(stmt,0),sqlite3_column_text(stmt,1)?(const char*)sqlite3_column_text(stmt,1):"null",sqlite3_column_text(stmt,2)?(const char*)sqlite3_column_text(stmt,2):"null");
    fprintf(out,",\"continuation\":%s,\"derived_summary\":",sqlite3_column_text(stmt,3)?(const char*)sqlite3_column_text(stmt,3):"null");
    const char *summary=(const char*)sqlite3_column_text(stmt,4);if(summary)json_quote(out,summary,strlen(summary));else fputs("null",out);fputs(",\"messages\":[",out);sqlite3_finalize(stmt);
    if(!prepare(s,&stmt,"SELECT role,content,operation FROM messages WHERE conversation=? ORDER BY sequence LIMIT 512"))return 1;
    text(stmt,1,id);int first=1;
    while(sqlite3_step(stmt)==SQLITE_ROW){if(!first)fputc(',',out);first=0;fputs("{\"role\":",out);const char *role=(const char*)sqlite3_column_text(stmt,0),*content=(const char*)sqlite3_column_text(stmt,1),*op=(const char*)sqlite3_column_text(stmt,2);json_quote(out,role,strlen(role));fputs(",\"content\":",out);json_quote(out,content,strlen(content));fputs(",\"operation_id\":",out);json_quote(out,op,strlen(op));fputc('}',out);}
    sqlite3_finalize(stmt);fputs("]}\n",out);return 0;
}
static int run_reply(Store *s,const char *operation,FILE *out) {
    sqlite3_stmt *stmt=NULL;if(!prepare(s,&stmt,"SELECT id,conversation,status,result,request,coalesce((SELECT cancel_requested FROM run_controls WHERE run_controls.owner=runs.owner AND run_controls.operation=runs.operation),0) FROM runs WHERE owner=? AND operation=?"))return fail(out,"store_failed");
    text(stmt,1,s->owner);text(stmt,2,operation);int rc=sqlite3_step(stmt);
    if(rc!=SQLITE_ROW){sqlite3_finalize(stmt);return fail(out,rc==SQLITE_DONE?"unknown_run":"store_failed");}
    fputs("{\"contract_version\":1,\"run_id\":",out);const char *id=(const char*)sqlite3_column_text(stmt,0);json_quote(out,id,strlen(id));
    fputs(",\"operation_id\":",out);json_quote(out,operation,strlen(operation));fputs(",\"conversation_id\":",out);
    const char *conversation=(const char*)sqlite3_column_text(stmt,1),*status=(const char*)sqlite3_column_text(stmt,2);json_quote(out,conversation,strlen(conversation));fputs(",\"status\":",out);json_quote(out,status,strlen(status));
    fprintf(out,",\"cancel_requested\":%s,\"input\":%s,\"result\":%s,\"children\":[",sqlite3_column_int(stmt,5)?"true":"false",(const char*)sqlite3_column_text(stmt,4),sqlite3_column_text(stmt,3)?(const char*)sqlite3_column_text(stmt,3):"null");sqlite3_finalize(stmt);stmt=NULL;
    if(!prepare(s,&stmt,"SELECT child_id,kind,status,cancellation FROM run_children WHERE owner=? AND operation=? ORDER BY ordinal LIMIT 16"))return 1;
    text(stmt,1,s->owner);text(stmt,2,operation);int first=1;
    while(sqlite3_step(stmt)==SQLITE_ROW){if(!first)fputc(',',out);first=0;const char *keys[]={"operation_id","kind","status","cancellation"};fputc('{',out);
        for(unsigned i=0;i<4;i++){if(i)fputc(',',out);json_quote(out,keys[i],strlen(keys[i]));fputc(':',out);const char *value=(const char*)sqlite3_column_text(stmt,(int)i);json_quote(out,value,strlen(value));}fputc('}',out);}
    sqlite3_finalize(stmt);fputs("]}\n",out);return 0;
}
static int saved_reply(Store *s,const char *operation,FILE *out) {
    if(!execsql(s,"BEGIN"))return fail(out,"store_busy");
    char *buffer=NULL;size_t n=0;FILE *f=open_memstream(&buffer,&n);
    if(!f){execsql(s,"ROLLBACK");return fail(out,"resource_limit");}
    int rc=run_reply(s,operation,f),closed=fclose(f);
    int committed=execsql(s,rc?"ROLLBACK":"COMMIT");
    if(closed||!committed){execsql(s,"ROLLBACK");rc=fail(out,"store_failed");}
    else fwrite(buffer,1,n,out);
    free(buffer);return rc;
}
static const char *child_observe(void *opaque,const char *child,const char *kind,const char *status,const char *cancellation) {
    RunControl *c=opaque;Store *s=c->store;sqlite3_stmt *stmt=NULL;
    if(!execsql(s,"BEGIN IMMEDIATE"))return "store_busy";
    int ok=prepare(s,&stmt,"SELECT kind FROM run_children WHERE owner=? AND operation=? AND child_id=?"),found=0;
    if(ok){text(stmt,1,s->owner);text(stmt,2,c->operation);text(stmt,3,child);int rc=sqlite3_step(stmt);found=rc==SQLITE_ROW;ok=found?!strcmp((const char*)sqlite3_column_text(stmt,0),kind):rc==SQLITE_DONE;}
    sqlite3_finalize(stmt);stmt=NULL;
    if(ok&&!found) {
        ok=prepare(s,&stmt,"INSERT INTO run_children(owner,operation,child_id,kind,status,cancellation,ordinal) SELECT ?,?,?,?,?,?,count(*) FROM run_children WHERE owner=? AND operation=? HAVING count(*)<16");
        if(ok){text(stmt,1,s->owner);text(stmt,2,c->operation);text(stmt,3,child);text(stmt,4,kind);text(stmt,5,status);text(stmt,6,cancellation);text(stmt,7,s->owner);text(stmt,8,c->operation);ok=sqlite3_step(stmt)==SQLITE_DONE&&sqlite3_changes(s->db)==1;}
    }else if(ok) {
        ok=prepare(s,&stmt,"UPDATE run_children SET status=?,cancellation=? WHERE owner=? AND operation=? AND child_id=?");
        if(ok){text(stmt,1,status);text(stmt,2,cancellation);text(stmt,3,s->owner);text(stmt,4,c->operation);text(stmt,5,child);ok=sqlite3_step(stmt)==SQLITE_DONE&&sqlite3_changes(s->db)==1;}
    }
    sqlite3_finalize(stmt);if(!ok||!execsql(s,"COMMIT")){execsql(s,"ROLLBACK");c->invalid=1;return "store_failed";}return NULL;
}
static int run_cancelled(void *opaque) {
    RunControl *c=opaque;sqlite3_stmt *stmt=NULL;
    if(c->upstream->cancelled&&c->upstream->cancelled(c->upstream->cancellation))return 1;
    int ok=prepare(c->store,&stmt,"SELECT cancel_requested FROM run_controls WHERE owner=? AND operation=?"),cancelled=0;
    if(ok){text(stmt,1,c->store->owner);text(stmt,2,c->operation);ok=sqlite3_step(stmt)==SQLITE_ROW;if(ok)cancelled=sqlite3_column_int(stmt,0)!=0;}
    sqlite3_finalize(stmt);if(!ok)c->invalid=1;return cancelled||!ok;
}
static int request_cancel(Store *s,const char *operation,FILE *out) {
    sqlite3_stmt *stmt=NULL;
    if(!prepare(s,&stmt,"UPDATE run_controls SET cancel_requested=1 WHERE owner=? AND operation=? AND EXISTS(SELECT 1 FROM runs WHERE runs.owner=run_controls.owner AND runs.operation=run_controls.operation AND runs.status='running')"))return fail(out,"store_failed");
    text(stmt,1,s->owner);text(stmt,2,operation);int ok=sqlite3_step(stmt)==SQLITE_DONE;sqlite3_finalize(stmt);
    return ok?run_reply(s,operation,out):fail(out,"store_failed");
}
static char *selector(const Json *j,int object) {
    const char *keys[]={"source_id","version","start_byte","end_byte","start_page","end_page"};
    char *out=NULL;size_t n;FILE *stream=open_memstream(&out,&n);if(!stream)return NULL;fputc('{',stream);int first=1;
    for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++){int token=json_get(j,object,keys[i]);if(token>=0){if(!first){fputc(',',stream);}first=0;json_quote(stream,keys[i],strlen(keys[i]));fputc(':',stream);if(j->tokens[token].type==JT_STRING){char *value=json_string(j,token);if(!value){fclose(stream);free(out);return NULL;}json_quote(stream,value,strlen(value));free(value);}else {size_t value;if(!json_uint(j,token,&value)){fclose(stream);free(out);return NULL;}fprintf(stream,"%zu",value);}}}
    fputc('}',stream);if(fclose(stream)){free(out);return NULL;}return out;
}
static char *canonical(const char *conversation,const char *operation,size_t revision,const char *question,const char *scope,size_t budget) {
    char *out=NULL;size_t n;FILE *stream=open_memstream(&out,&n);if(!stream)return NULL;
    fputs("{\"conversation_id\":",stream);json_quote(stream,conversation,strlen(conversation));fputs(",\"operation_id\":",stream);json_quote(stream,operation,strlen(operation));
    fprintf(stream,",\"expected_revision\":%zu,\"question\":",revision);json_quote(stream,question,strlen(question));fprintf(stream,",\"selector\":%s",scope?scope:"null");if(budget)fprintf(stream,",\"timeout_ms\":%zu",budget);fputc('}',stream);
    if(fclose(stream)){free(out);return NULL;}return out;
}
static const char *history(Store *s,const char *conversation,char **out) {
    *out=NULL;size_t n;FILE *stream=open_memstream(out,&n);if(!stream)return "resource_limit";
    sqlite3_stmt *stmt=NULL;const char *error=NULL;
    if(!prepare(s,&stmt,"SELECT messages.role,messages.content,runs.status FROM messages JOIN runs ON messages.conversation=runs.conversation AND messages.operation=runs.operation WHERE messages.conversation=? AND messages.sequence>(SELECT history_from FROM conversations WHERE id=messages.conversation) ORDER BY messages.sequence LIMIT 512"))error="store_failed";
    else text(stmt,1,conversation);
    fputc('[',stream);int first=1;
    sqlite3_stmt *summary_stmt=NULL;
    if(!error&&!prepare(s,&summary_stmt,"SELECT summary FROM conversations WHERE id=?"))error="store_failed";
    if(!error){text(summary_stmt,1,conversation);if(sqlite3_step(summary_stmt)!=SQLITE_ROW)error="store_failed";else {const char *summary=(const char*)sqlite3_column_text(summary_stmt,0);if(summary){fputs("{\"role\":\"derived_summary\",\"run_status\":\"completed\",\"content\":",stream);json_quote(stream,summary,strlen(summary));fputc('}',stream);first=0;}}}
    sqlite3_finalize(summary_stmt);

    while(!error) {
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *role=(const char*)sqlite3_column_text(stmt,0),*content=(const char*)sqlite3_column_text(stmt,1),*status=(const char*)sqlite3_column_text(stmt,2);
        if(ftell(stream)+(long)(strlen(content)*6+strlen(role)+strlen(status)+128)>65536){error="context_history_limit";break;}
        if(!first){fputc(',',stream);}first=0;fputs("{\"role\":",stream);json_quote(stream,role,strlen(role));fputs(",\"content\":",stream);json_quote(stream,content,strlen(content));fputs(",\"run_status\":",stream);json_quote(stream,status,strlen(status));fputc('}',stream);
    }
    sqlite3_finalize(stmt);fputc(']',stream);if(fclose(stream)&&!error)error="resource_limit";
    if(error){free(*out);*out=NULL;}return error;
}
static int message(Store *s,const char *conversation,const char *operation,const char *role,const char *content) {
    sqlite3_stmt *stmt=NULL;int ok=prepare(s,&stmt,"INSERT INTO messages(conversation,operation,role,content) VALUES(?,?,?,?)");
    if(ok){text(stmt,1,conversation);text(stmt,2,operation);text(stmt,3,role);text(stmt,4,content);ok=sqlite3_step(stmt)==SQLITE_DONE;}sqlite3_finalize(stmt);return ok;
}
static int chat_scope(const char *scope){Json j;if(!scope||!json_parse(&j,scope,strlen(scope))||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=1)return 0;char *kind=json_string(&j,json_get(&j,0,"kind"));int chat=kind&&!strcmp(kind,"chat");free(kind);return chat;}
static char *question_request(const char *scope,const char *question) {
    Json j;if(!json_parse(&j,scope,strlen(scope))||j.tokens[0].type!=JT_OBJECT)return NULL;
    int chat=chat_scope(scope);char *out=NULL;size_t n;FILE *stream=open_memstream(&out,&n);if(!stream)return NULL;fprintf(stream,"{\"contract_version\":1,\"op\":\"%s\",\"question\":",chat?"chat":"question");json_quote(stream,question,strlen(question));
    if(!chat)for(int t=1;t<j.tokens[0].next;t=j.tokens[j.tokens[t].next].next){fputc(',',stream);raw(stream,&j,t);fputc(':',stream);raw(stream,&j,j.tokens[t].next);}
    fputc('}',stream);
    if(fclose(stream)){free(out);return NULL;}return out;
}
static int submit(Store *s,const AgentConfig *cfg,const Json *request,int chat_mode,FILE *out) {
    const char *error=NULL;sqlite3_stmt *stmt=NULL;int transaction=0,admitted=0,pending_retry=0;
    char *conversation=json_string(request,json_get(request,0,"conversation_id")),*operation=json_string(request,json_get(request,0,"operation_id")),*question=json_string(request,json_get(request,0,"question"));size_t revision=0;
    char *new_scope=NULL,*old_scope=NULL,*old_evidence=NULL,*old_continuation=NULL,*next_continuation=NULL,*encoded=NULL,*past=NULL,*prepared=NULL,*response=NULL,*result=NULL,*selected_evidence=NULL,*assistant=NULL,*work_error=NULL;size_t response_length=0;
    int explicit_scope=json_get(request,0,"source_id")>=0||json_get(request,0,"version")>=0||json_get(request,0,"start_byte")>=0||json_get(request,0,"end_byte")>=0||json_get(request,0,"start_page")>=0||json_get(request,0,"end_page")>=0;
    if(!identity(conversation,64)||!identity(operation,128)||!question||!*question||strlen(question)>2048||!json_uint(request,json_get(request,0,"expected_revision"),&revision)||revision>512)error="invalid_request";
    if(!error&&chat_mode){new_scope=strdup("{\"kind\":\"chat\"}");explicit_scope=1;if(!new_scope)error="resource_limit";}
    else if(!error&&explicit_scope){new_scope=selector(request,0);if(!new_scope)error="invalid_selector";}
    size_t budget=0;int budget_token=json_get(request,0,"timeout_ms");
    if(!error&&budget_token>=0&&(!json_uint(request,budget_token,&budget)||!budget||budget>60000))error="invalid_request";
    AgentConfig work_cfg=*cfg;
    if(budget){double received=cfg->received_monotonic?cfg->received_monotonic:agent_now();if(received<0)error="state_unavailable";else{double deadline=received+(double)budget/1000;if(!work_cfg.deadline_monotonic||deadline<work_cfg.deadline_monotonic)work_cfg.deadline_monotonic=deadline;}}
    if(!error){encoded=canonical(conversation,operation,revision,question,new_scope,budget);if(!encoded)error="resource_limit";}
    /* Retry resolution precedes dependency setup and current revision checks. */
    if(!error) {
        if(!prepare(s,&stmt,"SELECT request,status FROM runs WHERE owner=? AND operation=?"))error="store_failed";
        else {text(stmt,1,s->owner);text(stmt,2,operation);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){const char *previous=(const char*)sqlite3_column_text(stmt,0);if(!previous||strcmp(previous,encoded))error="idempotency_conflict";else {admitted=1;pending_retry=!strcmp((const char*)sqlite3_column_text(stmt,1),"running");}}else if(rc!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error&&admitted) {
        if(pending_retry){const char *locked=lock_store(s);if(!locked)error=recover(s);else if(strcmp(locked,"busy"))error=locked;}
        goto done;
    }
    if(!error)error=agent_interrupted(&work_cfg);
    if(!error){error=lock_store(s);if(!error)error=recover(s);}
    if(!error){if(!execsql(s,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    if(!error) {
        if(!prepare(s,&stmt,"SELECT revision,scope,evidence,continuation FROM conversations WHERE id=? AND owner=?"))error="store_failed";
        else {
            text(stmt,1,conversation);text(stmt,2,s->owner);int rc=sqlite3_step(stmt);
            if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_conversation":"store_failed";
            else if((size_t)sqlite3_column_int64(stmt,0)!=revision)error="stale_revision";
            else {old_scope=copy_column(stmt,1);old_evidence=copy_column(stmt,2);old_continuation=copy_column(stmt,3);if(!explicit_scope){chat_mode=chat_scope(old_scope);if(!old_scope||(!chat_mode&&!old_evidence))error="selection_required";}}
        }
        sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error) {
        error=conversation_idle(s,conversation);
    }
    if(!error) {
        sqlite3_int64 runs=scalar(s,"SELECT count(*) FROM runs"),events=scalar(s,"SELECT count(*) FROM events"),bytes=usage(s),active=scalar(s,"SELECT count(*) FROM runs WHERE status='running'");
        if(runs<0||events<0||bytes<0||active<0)error="store_failed";
        else if(active>=ACTIVE_MAX)error="run_capacity";
        else if(runs>=256||events+active>510||bytes>SEMANTIC_MAX-(active+1)*3*RUN_RESULT_MAX)error="storage_limit";
    }
    if(!error) {
        if(!prepare(s,&stmt,"SELECT count(*) FROM messages WHERE conversation=?"))error="store_failed";
        else {text(stmt,1,conversation);if(sqlite3_step(stmt)!=SQLITE_ROW)error="store_failed";else if(sqlite3_column_int(stmt,0)>510)error="storage_limit";}sqlite3_finalize(stmt);stmt=NULL;
    }
    /* Assemble prior history before appending this turn; never duplicate user. */
    if(!error)error=history(s,conversation,&past);
    if(!error){prepared=question_request(explicit_scope?new_scope:old_scope,question);if(!prepared)error="invalid_selector";}
    if(!error&&!chat_mode) {
        Json validation;size_t lo,hi;char *source=NULL,*version=NULL;int pdf=0,bytes=0;
        if(!json_parse(&validation,prepared,strlen(prepared)))error="invalid_selector";
        else {
            source=json_string(&validation,json_get(&validation,0,"source_id"));version=json_string(&validation,json_get(&validation,0,"version"));
            pdf=json_get(&validation,0,"start_page")>=0||json_get(&validation,0,"end_page")>=0;bytes=json_get(&validation,0,"start_byte")>=0||json_get(&validation,0,"end_byte")>=0;
            if(!hash_identity(source)||!hash_identity(version)||pdf==bytes||!json_uint(&validation,json_get(&validation,0,pdf?"start_page":"start_byte"),&lo)||!json_uint(&validation,json_get(&validation,0,pdf?"end_page":"end_byte"),&hi)||hi<lo||(pdf?(lo<1||hi-lo>=4):hi-lo>65536))error="invalid_selector";
            free(source);free(version);
        }
    }
    if(!error) {
        char id[65];hash_bytes(encoded,strlen(encoded),id);
        error=worker_lease(s,id,&s->worker);
        if(!error)snprintf(s->worker_path,sizeof(s->worker_path),"run-%s.lock",id);
#ifdef SAMOSA_AGENT_TEST_FAULTS
        const char *lease_marker=getenv("SAMOSA_AGENT_TEST_LEASE_MARKER");
        if(!error&&lease_marker){int fd=open(lease_marker,O_CREAT|O_WRONLY|O_TRUNC,0600);if(fd<0)_exit(98);(void)write(fd,"leased",6);(void)fsync(fd);close(fd);for(;;)pause();}
#endif
        if(!error&&!prepare(s,&stmt,"INSERT INTO runs(id,owner,operation,conversation,request,status) VALUES(?,?,?,?,?,'running')"))error="store_failed";
        else if(!error){text(stmt,1,id);text(stmt,2,s->owner);text(stmt,3,operation);text(stmt,4,conversation);text(stmt,5,encoded);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
        if(!error) {
            if(!prepare(s,&stmt,"INSERT INTO run_controls(owner,operation) VALUES(?,?)"))error="store_failed";
            else {text(stmt,1,s->owner);text(stmt,2,operation);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
        }
    }
    if(!error) {
        if(!prepare(s,&stmt,"UPDATE conversations SET revision=revision+1 WHERE id=?"))error="store_failed";
        else {text(stmt,1,conversation);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
        if(!error&&(!message(s,conversation,operation,"user",question)||!event(s,conversation,operation,"running",revision+1)))error="store_failed";
    }
    if(!error)error=agent_interrupted(&work_cfg);
    if(transaction){if(error)execsql(s,"ROLLBACK");else if(!execsql(s,"COMMIT")){execsql(s,"ROLLBACK");error="store_failed";}transaction=0;}
    if(error)goto done;
    admitted=1;
    unlock_store(s); /* Provider work holds only this run's exact worker lease. */
#ifdef SAMOSA_AGENT_TEST_FAULTS
    const char *marker=getenv("SAMOSA_AGENT_TEST_RESERVED_MARKER");
    if(marker) {
        int fd=open(marker,O_CREAT|O_WRONLY|O_TRUNC,0600);
        if(fd<0)_exit(98);
        (void)write(fd,"reserved",8);(void)fsync(fd);close(fd);
        for(;;)pause(); /* Test-only actual process-kill barrier, after commit. */
    }
#endif
    FILE *stream=open_memstream(&response,&response_length);
    RunControl control={.store=s,.upstream=cfg,.operation=operation};
    work_cfg.cancelled=run_cancelled;work_cfg.cancellation=&control;
    char parent_id[65];hash_bytes(encoded,strlen(encoded),parent_id);
    work_cfg.parent_operation_id=parent_id;work_cfg.child_observer=child_observe;work_cfg.child_observer_context=&control;
    if(!stream)error="resource_limit";
    else {
        int rc=agent_replay_session(&work_cfg,prepared,strlen(prepared),explicit_scope?NULL:old_evidence,past,old_continuation,stream);
        if(fclose(stream))error="resource_limit";
        else if(rc) {
            Json failed;error="answer_failed";
            if(json_parse(&failed,response,response_length)) {
                work_error=json_string(&failed,json_get(&failed,json_get(&failed,0,"error"),"code"));
                if(identity(work_error,128))error=work_error;
            }
        }
    }
    Json answer;
    if(!error&&response_length>RUN_RESULT_MAX)error="result_too_large";
    if(!error&&(!json_parse(&answer,response,response_length)||json_get(&answer,0,"error")>=0))error="answer_failed";
    if(!error) {
        assistant=json_string(&answer,json_get(&answer,0,"answer"));int ev=json_get(&answer,0,"admitted_evidence");
        if(!assistant||strlen(assistant)>65536||ev<0||answer.tokens[ev].type!=JT_ARRAY||answer.tokens[ev].count!=(chat_mode?0:1))error="invalid_result";
        else if(!chat_mode) {
            int item=ev+1;size_t n=answer.tokens[item].end-answer.tokens[item].begin;selected_evidence=malloc(n+1);if(!selected_evidence)error="resource_limit";else {memcpy(selected_evidence,response+answer.tokens[item].begin,n);selected_evidence[n]=0;}
        }
    }
    if(!error) { int ref=json_get(&answer,0,"continuation"); if(ref<0||answer.tokens[ref].type!=JT_OBJECT)error="invalid_result"; else { size_t n=answer.tokens[ref].end-answer.tokens[ref].begin; next_continuation=malloc(n+1); if(!next_continuation)error="resource_limit"; else { memcpy(next_continuation,response+answer.tokens[ref].begin,n);next_continuation[n]=0; } } }
    /* Serialize the last cancellation observation with terminal publication. */
    if(!execsql(s,"BEGIN IMMEDIATE")){error="store_failed";goto done;}transaction=1;
    const char *interruption=agent_interrupted(&work_cfg);
    if(interruption)error=control.invalid?"store_failed":interruption;
    if(control.invalid)error="store_failed";
    if(!error) {
        if(!prepare(s,&stmt,"SELECT revision FROM conversations WHERE id=? AND owner=?"))error="store_failed";
        else {text(stmt,1,conversation);text(stmt,2,s->owner);if(sqlite3_step(stmt)!=SQLITE_ROW||(size_t)sqlite3_column_int64(stmt,0)!=revision+1)error="stale_revision";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    size_t result_length=0;
    stream=open_memstream(&result,&result_length);
    if(!stream)error="resource_limit";
    else {
        if(error){fputs("{\"error\":{\"code\":",stream);json_quote(stream,error,strlen(error));fprintf(stream,"},\"revision\":%zu}",revision+1);}
        else {
            fprintf(stream,"{\"revision\":%zu",revision+2);const char *keys[]={"answer","citations","admitted_evidence","usage","context_policy","continuation"};
            for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++){int token=json_get(&answer,0,keys[i]);if(token<0){error="invalid_result";break;}fputc(',',stream);json_quote(stream,keys[i],strlen(keys[i]));fputc(':',stream);raw(stream,&answer,token);}fputc('}',stream);
        }
        if(fclose(stream))error="resource_limit";
    }
    const char *status=error?(!strcmp(error,"cancelled")?"cancelled":"failed"):"completed";
    if(!result){error="store_failed";goto done;}
    if(!error) {
        if(!prepare(s,&stmt,"UPDATE conversations SET revision=revision+1,scope=?,evidence=?,continuation=? WHERE id=?"))error="store_failed";
        else {text(stmt,1,explicit_scope?new_scope:old_scope);text(stmt,2,selected_evidence);text(stmt,3,next_continuation);text(stmt,4,conversation);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
        if(!error&&!message(s,conversation,operation,"assistant",assistant))error="store_failed";
    }
    int ok=prepare(s,&stmt,"UPDATE runs SET status=?,result=? WHERE owner=? AND operation=?");
    if(ok){text(stmt,1,status);text(stmt,2,result);text(stmt,3,s->owner);text(stmt,4,operation);ok=sqlite3_step(stmt)==SQLITE_DONE;}sqlite3_finalize(stmt);stmt=NULL;
    if(ok)ok=event(s,conversation,operation,status,revision+(!strcmp(status,"completed")?2:1));
    if(!ok||(!strcmp(status,"completed")&&error)||!execsql(s,"COMMIT")){execsql(s,"ROLLBACK");error="store_failed";}else error=NULL;transaction=0;
done:
    if(transaction)execsql(s,"ROLLBACK");
    if(!admitted&&s->worker>=0) {
        /* Failed admission cannot accumulate lease files outside run history.
         * Keep the admission gate and exact lease until removal is complete. */
        struct stat held,named;
        if(fstat(s->worker,&held)==0&&fstatat(s->directory,s->worker_path,&named,AT_SYMLINK_NOFOLLOW)==0&&held.st_dev==named.st_dev&&held.st_ino==named.st_ino)
            (void)unlinkat(s->directory,s->worker_path,0);
    }
    int rc=error?fail(out,error):saved_reply(s,operation,out);
    free(conversation);free(operation);free(question);free(new_scope);free(old_scope);free(old_evidence);free(old_continuation);free(next_continuation);free(encoded);free(past);free(prepared);free(response);free(result);free(selected_evidence);free(assistant);free(work_error);return rc;
}
/* Compaction is a bounded, derived excerpt of owned messages. Original messages
 * and literal source evidence remain authoritative and are never deleted. */
static int compact(Store *s,const Json *request,FILE *out) {
    char *id=json_string(request,json_get(request,0,"conversation_id"));
    char *operation=json_string(request,json_get(request,0,"operation_id"));
    size_t revision=0;const char *error=NULL;sqlite3_stmt *stmt=NULL;
    char *encoded=NULL,*summary=NULL,*pin=NULL;size_t n=0;int transaction=0,retry=0;
    if(!identity(id,64)||!identity(operation,128)||!json_uint(request,json_get(request,0,"expected_revision"),&revision)||revision>512)error="invalid_request";
    if(!error){encoded=canonical(id,operation,revision,"",NULL,0);if(!encoded)error="resource_limit";}
    /* Operation kind participates in the idempotency identity. */
    if(!error){size_t len=strlen(encoded);char *tagged=malloc(len+32);if(!tagged)error="resource_limit";else {snprintf(tagged,len+32,"{\"kind\":\"compact\",\"input\":%s}",encoded);free(encoded);encoded=tagged;}}
    if(!error){if(!prepare(s,&stmt,"SELECT request FROM runs WHERE owner=? AND operation=?"))error="store_failed";else {text(stmt,1,s->owner);text(stmt,2,operation);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){retry=1;if(strcmp((const char*)sqlite3_column_text(stmt,0),encoded))error="idempotency_conflict";}else if(rc!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error&&retry)goto done;
    if(!error){error=lock_store(s);if(!error)error=recover(s);}
    if(!error){if(!execsql(s,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    if(!error){if(!prepare(s,&stmt,"SELECT revision,continuation FROM conversations WHERE id=? AND owner=?"))error="store_failed";else {text(stmt,1,id);text(stmt,2,s->owner);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_conversation":"store_failed";else if((size_t)sqlite3_column_int64(stmt,0)!=revision)error="stale_revision";
            else {const char *ref=(const char*)sqlite3_column_text(stmt,1);if(ref){Json j;size_t context=0;char *provider=NULL;if(!json_parse(&j,ref,strlen(ref)))error="invalid_session";else {provider=json_string(&j,json_get(&j,0,"provider_id"));if(!provider||!json_uint(&j,json_get(&j,0,"context_tokens"),&context))error="invalid_session";else {size_t length=0;FILE *stream=open_memstream(&pin,&length);if(!stream)error="resource_limit";else {fputs("{\"provider_id\":",stream);json_quote(stream,provider,strlen(provider));fprintf(stream,",\"context_tokens\":%zu}",context);if(fclose(stream))error="resource_limit";}}}free(provider);}}
        }sqlite3_finalize(stmt);stmt=NULL;}

    if(!error)error=conversation_idle(s,id);
    if(!error){sqlite3_int64 bytes=usage(s),runs=scalar(s,"SELECT count(*) FROM runs"),events=scalar(s,"SELECT count(*) FROM events"),active=scalar(s,"SELECT count(*) FROM runs WHERE status='running'");if(bytes<0||runs<0||events<0||active<0)error="store_failed";else if(bytes>SEMANTIC_MAX-active*3*RUN_RESULT_MAX-65536||runs>=256||events+active>=512)error="storage_limit";}
    sqlite3_int64 boundary=0;size_t included=0,omitted=0;
    if(!error){FILE *stream=open_memstream(&summary,&n);if(!stream)error="resource_limit";else {
        fputs("Derived message excerpts; these are conversation history, not literal source evidence.\n",stream);
        if(!prepare(s,&stmt,"SELECT sequence,role,content FROM messages WHERE conversation=? ORDER BY sequence LIMIT 512"))error="store_failed";
        else text(stmt,1,id);
        while(!error){int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
            boundary=sqlite3_column_int64(stmt,0);const char *role=(const char*)sqlite3_column_text(stmt,1),*content=(const char*)sqlite3_column_text(stmt,2);
            size_t length=strlen(content);if(length>128){length=128;while(length&&((unsigned char)content[length]&0xc0)==0x80)length--;}
            if(ftell(stream)>7600){omitted++;continue;}
            fprintf(stream,"message %lld %s: ",(long long)boundary,role);fwrite(content,1,length,stream);if(length<strlen(content))fputs(" [excerpt]",stream);fputc('\n',stream);included++;
        }
        sqlite3_finalize(stmt);stmt=NULL;fprintf(stream,"Included %zu message excerpts; omitted %zu messages.\n",included,omitted);if(fclose(stream)&&!error)error="resource_limit";
    }}
    if(!error){if(!prepare(s,&stmt,"UPDATE conversations SET revision=revision+1,summary=?,history_from=?,continuation=? WHERE id=?"))error="store_failed";else {text(stmt,1,summary);sqlite3_bind_int64(stmt,2,boundary);if(pin)text(stmt,3,pin);else sqlite3_bind_null(stmt,3);text(stmt,4,id);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error){char run[65],result[256];hash_bytes(encoded,strlen(encoded),run);snprintf(result,sizeof(result),"{\"revision\":%zu,\"compacted\":true,\"session_reset\":true,\"included_message_excerpts\":%zu,\"omitted_message_excerpts\":%zu}",revision+1,included,omitted);
        if(!prepare(s,&stmt,"INSERT INTO runs(id,owner,operation,conversation,request,status,result) VALUES(?,?,?,?,?,'completed',?)"))error="store_failed";else {text(stmt,1,run);text(stmt,2,s->owner);text(stmt,3,operation);text(stmt,4,id);text(stmt,5,encoded);text(stmt,6,result);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
        if(!error&&!event(s,id,operation,"completed",revision+1))error="store_failed";
    }
    if(transaction){if(error)execsql(s,"ROLLBACK");else if(!execsql(s,"COMMIT")){execsql(s,"ROLLBACK");error="store_failed";}transaction=0;}
done:
    free(id);free(encoded);free(summary);free(pin);int rc=error?fail(out,error):saved_reply(s,operation,out);free(operation);return rc;
}

static int events(Store *s,const char *id,size_t after,FILE *out) {
    sqlite3_stmt *stmt=NULL;if(!prepare(s,&stmt,"SELECT 1 FROM conversations WHERE id=? AND owner=?"))return fail(out,"store_failed");
    text(stmt,1,id);text(stmt,2,s->owner);int rc=sqlite3_step(stmt);sqlite3_finalize(stmt);if(rc!=SQLITE_ROW)return fail(out,rc==SQLITE_DONE?"unknown_conversation":"store_failed");
    if(!prepare(s,&stmt,"SELECT sequence,operation,status,revision FROM events WHERE conversation=? AND sequence>? ORDER BY sequence LIMIT 100"))return fail(out,"store_failed");
    text(stmt,1,id);sqlite3_bind_int64(stmt,2,(sqlite3_int64)after);fputs("{\"contract_version\":1,\"events\":[",out);int first=1;
    while((rc=sqlite3_step(stmt))==SQLITE_ROW){if(!first)fputc(',',out);first=0;fprintf(out,"{\"sequence\":%lld,\"operation_id\":",(long long)sqlite3_column_int64(stmt,0));const char *op=(const char*)sqlite3_column_text(stmt,1),*status=(const char*)sqlite3_column_text(stmt,2);json_quote(out,op,strlen(op));fputs(",\"status\":",out);json_quote(out,status,strlen(status));fprintf(out,",\"revision\":%lld}",(long long)sqlite3_column_int64(stmt,3));}
    sqlite3_finalize(stmt);fputs("]}\n",out);return rc==SQLITE_DONE?0:1;
}
int store_dispatch(const AgentConfig *cfg,const char *input,size_t length,FILE *out) {
    AgentConfig received={0};if(cfg){received=*cfg;if(!received.received_monotonic)received.received_monotonic=agent_now();cfg=&received;}
    if(length>AGENT_REQUEST_MAX)return fail(out,"request_too_large");
    Json request;size_t version;
    if(!json_parse(&request,input,length)||request.tokens[0].type!=JT_OBJECT)return fail(out,"invalid_json");
    if(!json_uint(&request,json_get(&request,0,"contract_version"),&version)||version!=1)return fail(out,"unsupported_contract");
    char *op=json_string(&request,json_get(&request,0,"op"));if(!op)return fail(out,"invalid_request");
    const char *fields=!strcmp(op,"create")?"|op|contract_version|":!strcmp(op,"get")?"|op|contract_version|conversation_id|":!strcmp(op,"events")?"|op|contract_version|conversation_id|after|":(!strcmp(op,"get_run")||!strcmp(op,"cancel_run"))?"|op|contract_version|operation_id|run_id|":!strcmp(op,"compact")?"|op|contract_version|conversation_id|expected_revision|operation_id|":!strcmp(op,"submit_chat")?"|op|contract_version|conversation_id|expected_revision|operation_id|question|timeout_ms|":!strcmp(op,"submit")?"|op|contract_version|conversation_id|expected_revision|operation_id|question|source_id|version|start_byte|end_byte|start_page|end_page|timeout_ms|":NULL;
    if(!fields){free(op);return fail(out,"unsupported_operation");}
    for(int t=1;t<request.tokens[0].next;t=request.tokens[request.tokens[t].next].next){char *key=json_string(&request,t),match[128];int n=snprintf(match,sizeof(match),"|%s|",key?key:"");free(key);if(n<0||(size_t)n>=sizeof(match)||!strstr(fields,match)){free(op);return fail(out,"unknown_field");}}
    Store s;const char *error=open_store(&s,cfg);int rc=1;
    if(error){close_store(&s);free(op);return fail(out,error);}
    if(!strcmp(op,"create"))rc=create(&s,out);
    else if(!strcmp(op,"submit")||!strcmp(op,"submit_chat"))rc=submit(&s,cfg,&request,!strcmp(op,"submit_chat"),out);
    else if(!strcmp(op,"compact"))rc=compact(&s,&request,out);
    else {
        /* Recovery checks exact leases under the short admission gate. Readers
         * remain available when another process briefly holds that gate. */
        const char *locked=lock_store(&s);if(!locked)error=recover(&s);else if(strcmp(locked,"busy"))error=locked;
        unlock_store(&s);
        char *id=json_string(&request,json_get(&request,0,(!strcmp(op,"get_run")||!strcmp(op,"cancel_run"))?"operation_id":"conversation_id"));
        if(!strcmp(op,"get_run")||!strcmp(op,"cancel_run")) {
            char *run=json_string(&request,json_get(&request,0,"run_id"));
            if((id&&run)||(!id&&!run))error="invalid_request";
            if(!error&&run) {
                sqlite3_stmt *stmt=NULL;if(!identity(run,64)||strlen(run)!=64)error="invalid_request";
                else if(!prepare(&s,&stmt,"SELECT operation FROM runs WHERE id=? AND owner=?"))error="store_failed";
                else {text(stmt,1,run);text(stmt,2,s.owner);int result=sqlite3_step(stmt);if(result==SQLITE_ROW)id=copy_column(stmt,0);else error=result==SQLITE_DONE?"unknown_run":"store_failed";}sqlite3_finalize(stmt);
            }
            free(run);
        }
        if(!error&&!identity(id,(!strcmp(op,"get_run")||!strcmp(op,"cancel_run"))?128:64))error="invalid_request";
        if(error)rc=fail(out,error);
        else if(!execsql(&s,!strcmp(op,"cancel_run")?"BEGIN IMMEDIATE":"BEGIN"))rc=fail(out,"store_busy");
        else {
            char *buffer=NULL;size_t n=0;FILE *stream=open_memstream(&buffer,&n);
            if(!stream){execsql(&s,"ROLLBACK");rc=fail(out,"resource_limit");}
            else {
                if(!strcmp(op,"cancel_run"))rc=request_cancel(&s,id,stream);
                else if(!strcmp(op,"get_run"))rc=run_reply(&s,id,stream);
                else if(!strcmp(op,"get"))rc=get(&s,id,stream);
                else {size_t after=0;int token=json_get(&request,0,"after");if(token>=0&&(!json_uint(&request,token,&after)||after>INT64_MAX))rc=fail(stream,"invalid_request");else rc=events(&s,id,after,stream);}
                int closed=fclose(stream);int committed=execsql(&s,rc?"ROLLBACK":"COMMIT");
                if(closed||!committed){execsql(&s,"ROLLBACK");rc=fail(out,"store_failed");}
                else fwrite(buffer,1,n,out); /* Release SQL snapshot before slow output. */
                free(buffer);
            }
        }
        free(id);
    }
    close_store(&s);free(op);return rc;
}
