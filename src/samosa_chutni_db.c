/* Network-free Chutni SQLite/FTS5 sidecar (T4.1--T4.5 core).
 *
 * This process owns only derived memory and receives a gateway-authorized
 * scope root. It never follows symlinks or stores source token-ID blobs.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "json.h"
#include "tok.h"
#include "sqlite/sqlite3.h"

#define SCHEMA_VERSION 2
#define CHUNKER_FINGERPRINT "chutni-chunker-v1-600-800"
#define DEFAULT_MAX_FILE_BYTES (256ULL * 1024ULL * 1024ULL)
#define MAX_BUILD_FILES 1000000ULL

static volatile sig_atomic_t build_canceled = 0;
static void on_build_signal(int sig) { (void)sig; build_canceled = 1; }

static int exec_sql(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
    }
    return rc == SQLITE_OK;
}

static int migrate(sqlite3 *db) {
    int version = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (version > SCHEMA_VERSION) { fprintf(stderr, "sqlite: unsupported schema version %d\n", version); return 0; }
    if (version == SCHEMA_VERSION) return 1;
    const char *sql =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tokenizer_profiles ("
        " profile_id TEXT PRIMARY KEY, model_id TEXT NOT NULL, model_version TEXT NOT NULL,"
        " tokenizer_sha256 TEXT NOT NULL, vocabulary_size INTEGER NOT NULL,"
        " special_token_policy TEXT NOT NULL, prompt_template_fingerprint TEXT NOT NULL,"
        " parity_corpus_version TEXT NOT NULL, direct_ingestion_state TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS files ("
        " file_id TEXT PRIMARY KEY, relative_path TEXT NOT NULL UNIQUE, stable_identity TEXT,"
        " path_bytes BLOB, display_path TEXT, size_bytes INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, content_sha256 TEXT,"
        " media_type TEXT NOT NULL, status TEXT NOT NULL, skip_reason TEXT, generation INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS contents ("
        " content_sha256 TEXT PRIMARY KEY, extraction_ref TEXT NOT NULL, parser_fingerprint TEXT NOT NULL,"
        " ocr_fingerprint TEXT NOT NULL, extracted_text TEXT, text_bytes INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS chunks ("
        " chunk_id TEXT PRIMARY KEY, file_id TEXT NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,"
        " ordinal INTEGER NOT NULL, relative_path TEXT NOT NULL, title TEXT NOT NULL DEFAULT '',"
        " page INTEGER, section TEXT, start_offset INTEGER NOT NULL, end_offset INTEGER NOT NULL,"
        " text TEXT NOT NULL, token_count INTEGER NOT NULL, tokenizer_fingerprint TEXT NOT NULL,"
        " generation INTEGER NOT NULL, UNIQUE(file_id, ordinal, generation));"
        "CREATE TABLE IF NOT EXISTS memory_cards ("
        " card_id TEXT PRIMARY KEY, text TEXT NOT NULL, source_chunk_id TEXT NOT NULL REFERENCES chunks(chunk_id) ON DELETE CASCADE,"
        " generator_model_id TEXT NOT NULL, prompt_version TEXT NOT NULL, generation INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS directory_summaries ("
        " summary_id TEXT PRIMARY KEY, relative_directory TEXT NOT NULL, text TEXT NOT NULL,"
        " source_chunk_id TEXT NOT NULL REFERENCES chunks(chunk_id) ON DELETE CASCADE,"
        " generator_model_id TEXT NOT NULL, prompt_version TEXT NOT NULL, generation INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS scope_meta ("
        " scope_id TEXT PRIMARY KEY, schema_version INTEGER NOT NULL, kind TEXT NOT NULL, display_name TEXT NOT NULL,"
        " canonical_root TEXT NOT NULL, volume_identity TEXT NOT NULL, root_file_identity TEXT NOT NULL,"
        " policy_json TEXT NOT NULL, policy_fingerprint TEXT NOT NULL, state TEXT NOT NULL,"
        " freshness_state TEXT NOT NULL, evidence_generation INTEGER NOT NULL, enhancement_revision INTEGER NOT NULL,"
        " current_job_id TEXT, last_successful_build_at TEXT, last_successful_check_at TEXT,"
        " regular_files_seen INTEGER NOT NULL DEFAULT 0, files_indexed INTEGER NOT NULL DEFAULT 0,"
        " files_skipped INTEGER NOT NULL DEFAULT 0, chunks_indexed INTEGER NOT NULL DEFAULT 0,"
        " source_bytes_indexed INTEGER NOT NULL DEFAULT 0, extracted_text_bytes INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS publications ("
        " generation INTEGER PRIMARY KEY, state TEXT NOT NULL, created_at TEXT NOT NULL,"
        " committed_at TEXT, database_name TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS manifest ("
        " file_id TEXT PRIMARY KEY REFERENCES files(file_id) ON DELETE CASCADE, path_bytes BLOB NOT NULL,"
        " stable_identity TEXT, size_bytes INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, content_sha256 TEXT,"
        " media_type TEXT NOT NULL, status TEXT NOT NULL, skip_reason TEXT, generation INTEGER NOT NULL);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5("
        " chunk_id UNINDEXED, relative_path, title, text, tokenize='unicode61 remove_diacritics 2');"
        "INSERT OR REPLACE INTO metadata(key,value) VALUES"
        " ('schema_version','2'),('journal_mode','WAL'),('token_id_storage','forbidden'),"
        " ('chunker_fingerprint','" CHUNKER_FINGERPRINT "');"
        "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (2, strftime('%Y-%m-%dT%H:%M:%fZ','now'));"
        "PRAGMA user_version=2;"
        "COMMIT;";
    if (version == 1) {
        /* Databases produced by T4.1 remain readable. SQLite has no IF NOT
           EXISTS form for ADD COLUMN, so the version gate makes this safe. */
        const char *upgrade =
            "BEGIN IMMEDIATE;"
            "ALTER TABLE files ADD COLUMN path_bytes BLOB;"
            "ALTER TABLE files ADD COLUMN display_path TEXT;"
            "ALTER TABLE contents ADD COLUMN extracted_text TEXT;"
            "ALTER TABLE contents ADD COLUMN text_bytes INTEGER NOT NULL DEFAULT 0;"
            "CREATE TABLE IF NOT EXISTS scope_meta (scope_id TEXT PRIMARY KEY, schema_version INTEGER NOT NULL, kind TEXT NOT NULL, display_name TEXT NOT NULL, canonical_root TEXT NOT NULL, volume_identity TEXT NOT NULL, root_file_identity TEXT NOT NULL, policy_json TEXT NOT NULL, policy_fingerprint TEXT NOT NULL, state TEXT NOT NULL, freshness_state TEXT NOT NULL, evidence_generation INTEGER NOT NULL, enhancement_revision INTEGER NOT NULL, current_job_id TEXT, last_successful_build_at TEXT, last_successful_check_at TEXT, regular_files_seen INTEGER NOT NULL DEFAULT 0, files_indexed INTEGER NOT NULL DEFAULT 0, files_skipped INTEGER NOT NULL DEFAULT 0, chunks_indexed INTEGER NOT NULL DEFAULT 0, source_bytes_indexed INTEGER NOT NULL DEFAULT 0, extracted_text_bytes INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS publications (generation INTEGER PRIMARY KEY, state TEXT NOT NULL, created_at TEXT NOT NULL, committed_at TEXT, database_name TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS manifest (file_id TEXT PRIMARY KEY REFERENCES files(file_id) ON DELETE CASCADE, path_bytes BLOB NOT NULL, stable_identity TEXT, size_bytes INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, content_sha256 TEXT, media_type TEXT NOT NULL, status TEXT NOT NULL, skip_reason TEXT, generation INTEGER NOT NULL);"
            "INSERT OR REPLACE INTO metadata(key,value) VALUES('schema_version','2'),('chunker_fingerprint','" CHUNKER_FINGERPRINT "');"
            "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (2, strftime('%Y-%m-%dT%H:%M:%fZ','now'));"
            "PRAGMA user_version=2; COMMIT;";
        return exec_sql(db, upgrade);
    }
    return exec_sql(db, sql);
}

static sqlite3 *open_db(const char *path, int create) {
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) { if (db) sqlite3_close(db); return NULL; }
    sqlite3_busy_timeout(db, 5000);
    if (!exec_sql(db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;")) { sqlite3_close(db); return NULL; }
    if (!migrate(db)) { sqlite3_close(db); return NULL; }
    return db;
}

static int bind_text(sqlite3_stmt *stmt, int index, const char *value) {
    return sqlite3_bind_text(stmt, index, value ? value : "", -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

static int add_file(sqlite3 *db, int argc, char **argv) {
    if (argc != 10) return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO files(file_id,relative_path,stable_identity,size_bytes,mtime_ns,content_sha256,media_type,status,skip_reason,generation) VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(file_id) DO UPDATE SET relative_path=excluded.relative_path,stable_identity=excluded.stable_identity,size_bytes=excluded.size_bytes,mtime_ns=excluded.mtime_ns,content_sha256=excluded.content_sha256,media_type=excluded.media_type,status=excluded.status,skip_reason=excluded.skip_reason,generation=excluded.generation";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int ok = bind_text(stmt, 1, argv[2]) && bind_text(stmt, 2, argv[3]) && bind_text(stmt, 3, argv[4]);
    if (ok) ok = sqlite3_bind_int64(stmt, 4, atoll(argv[5])) == SQLITE_OK && sqlite3_bind_int64(stmt, 5, atoll(argv[6])) == SQLITE_OK;
    if (ok) ok = bind_text(stmt, 6, argv[7]) && bind_text(stmt, 7, argv[8]) && bind_text(stmt, 8, argv[9]);
    if (ok) ok = bind_text(stmt, 9, "") && sqlite3_bind_int64(stmt, 10, 1) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}

static int add_chunk(sqlite3 *db, int argc, char **argv) {
    if (argc != 14) return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR REPLACE INTO chunks(chunk_id,file_id,ordinal,relative_path,title,page,section,start_offset,end_offset,text,token_count,tokenizer_fingerprint,generation) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,1)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int ok = bind_text(stmt, 1, argv[2]) && bind_text(stmt, 2, argv[3]) && sqlite3_bind_int(stmt, 3, atoi(argv[4])) == SQLITE_OK;
    if (ok) ok = bind_text(stmt, 4, argv[5]) && bind_text(stmt, 5, argv[6]);
    if (ok) ok = sqlite3_bind_int(stmt, 6, atoi(argv[7])) == SQLITE_OK && sqlite3_bind_int(stmt, 7, atoi(argv[8])) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 8, atoi(argv[9])) == SQLITE_OK && sqlite3_bind_int(stmt, 9, atoi(argv[10])) == SQLITE_OK;
    if (ok) ok = bind_text(stmt, 10, argv[11]) && sqlite3_bind_int(stmt, 11, atoi(argv[12])) == SQLITE_OK && bind_text(stmt, 12, "qwen36b-text-v1");
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return 0;
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO chunks_fts(rowid,chunk_id,relative_path,title,text) SELECT rowid,chunk_id,relative_path,title,text FROM chunks WHERE chunk_id=?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    ok = bind_text(stmt, 1, argv[2]) && sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}

static int query_db(sqlite3 *db, const char *query, int limit) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT c.chunk_id,c.relative_path,c.text,bm25(chunks_fts,6.0,4.0,1.0) AS rank FROM chunks_fts JOIN chunks c ON c.rowid=chunks_fts.rowid WHERE chunks_fts MATCH ?1 ORDER BY rank ASC,c.relative_path ASC,c.chunk_id ASC LIMIT ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK || bind_text(stmt, 1, query) != 1 || sqlite3_bind_int(stmt, 2, limit) != SQLITE_OK) { sqlite3_finalize(stmt); return 0; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%s\t%s\t%.9f\t%s\n", sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1), sqlite3_column_double(stmt, 3), sqlite3_column_text(stmt, 2));
    }
    sqlite3_finalize(stmt); return 1;
}

static int integrity_db(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL; int ok = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) ok = !strcmp((const char *)sqlite3_column_text(stmt, 0), "ok");
    sqlite3_finalize(stmt);
    if (ok) ok = exec_sql(db, "INSERT INTO chunks_fts(chunks_fts) VALUES('integrity-check');") && exec_sql(db, "PRAGMA foreign_key_check;");
    if (ok) { int log_frames = 0, checkpointed = 0; sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &log_frames, &checkpointed); }
    puts(ok ? "{\"ok\":true,\"quick_check\":\"ok\",\"fts\":\"ok\",\"wal\":true}" : "{\"ok\":false,\"error\":\"index_corrupt\"}");
    return ok;
}

/* -------------------------------------------------------------------------
 * T4.2--T4.5 scope storage.  The browser gateway owns authorization and job
 * admission; this sidecar owns the durable, replayable evidence boundary.
 * A build always writes a new database and changes active.json only after all
 * validation succeeds.  That makes a killed writer boring: the old pointer
 * still names the last complete generation.
 */

typedef struct {
    char state_dir[PATH_MAX], scope_dir[PATH_MAX], scope_id[96];
    char kind[16], display_name[256], root[PATH_MAX];
    char volume_identity[96], root_file_identity[96], policy_fingerprint[65];
    char extractor[PATH_MAX], ocr[PATH_MAX];
    unsigned long long max_file_bytes;
    int include_hidden, cross_filesystems;
} Scope;

typedef struct { char *data; size_t len, cap; } StringBuffer;

static int sb_put(StringBuffer *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return 0;
        b->data = p; b->cap = cap;
    }
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
    return 1;
}

static int sb_json(StringBuffer *b, const char *s) {
    if (!sb_put(b, "\"")) return 0;
    for (; s && *s; ++s) {
        char escaped[7];
        switch ((unsigned char)*s) {
        case '"': if (!sb_put(b, "\\\"")) return 0; break;
        case '\\': if (!sb_put(b, "\\\\")) return 0; break;
        case '\n': if (!sb_put(b, "\\n")) return 0; break;
        case '\r': if (!sb_put(b, "\\r")) return 0; break;
        case '\t': if (!sb_put(b, "\\t")) return 0; break;
        default:
            if ((unsigned char)*s < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char)*s);
                if (!sb_put(b, escaped)) return 0;
            } else {
                char one[2] = {*s, 0};
                if (!sb_put(b, one)) return 0;
            }
        }
    }
    return sb_put(b, "\"");
}

static int mkdir_one(const char *path) {
    if (!mkdir(path, 0700) || errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }
    return 0;
}

static int mkdir_parents(const char *path) {
    char copy[PATH_MAX]; size_t i;
    if (!path || strlen(path) >= sizeof(copy)) return 0;
    strcpy(copy, path);
    for (i = 1; copy[i]; ++i) {
        if (copy[i] == '/') { copy[i] = 0; if (*copy && !mkdir_one(copy)) return 0; copy[i] = '/'; }
    }
    return mkdir_one(copy);
}

static int path_join_local(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n >= 0 && (size_t)n < cap;
}

static int fsync_parent(const char *path) {
    char parent[PATH_MAX], *slash;
    if (strlen(path) >= sizeof(parent)) return 0;
    strcpy(parent, path); slash = strrchr(parent, '/');
    if (!slash) return 1;
    *slash = slash == parent ? '/' : 0;
    int fd = open(parent, O_RDONLY);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0; close(fd); return ok;
}

static int atomic_text(const char *path, const char *text) {
    char tmp[PATH_MAX]; int fd; size_t off = 0, len = strlen(text);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) return 0;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    while (off < len) { ssize_t n = write(fd, text + off, len - off); if (n <= 0) { close(fd); unlink(tmp); return 0; } off += (size_t)n; }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0 || !fsync_parent(path)) { unlink(tmp); return 0; }
    return 1;
}

static char *read_all_local(const char *path) {
    FILE *f = fopen(path, "rb"); long n; char *p;
    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { if (f) fclose(f); return NULL; }
    p = malloc((size_t)n + 1); if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    p[n] = 0; fclose(f); return p;
}

static const char *json_string_field(jval *root, const char *key) {
    jval *v = root && root->t == J_OBJ ? json_get(root, key) : NULL;
    return v && v->t == J_STR ? v->str : NULL;
}

static int valid_scope_id(const char *s) {
    size_t i, n = s ? strlen(s) : 0;
    if (!n || n >= 80) return 0;
    for (i = 0; i < n; ++i) if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_')) return 0;
    return 1;
}

static int load_scope(const char *state_dir, const char *id, Scope *s) {
    char path[PATH_MAX], *raw = NULL, *arena = NULL; jval *root = NULL;
    const char *v;
    memset(s, 0, sizeof(*s));
    if (!valid_scope_id(id) || strlen(state_dir) >= PATH_MAX) return 0;
    snprintf(s->state_dir, sizeof(s->state_dir), "%s", state_dir);
    snprintf(s->scope_id, sizeof(s->scope_id), "%s", id);
    if (!path_join_local(s->scope_dir, sizeof(s->scope_dir), state_dir, "scopes") ||
        !path_join_local(s->scope_dir, sizeof(s->scope_dir), s->scope_dir, id) ||
        !path_join_local(path, sizeof(path), s->scope_dir, "scope.json")) return 0;
    raw = read_all_local(path); if (!raw) return 0;
    root = json_parse(raw, &arena);
    v = json_string_field(root, "kind"); if (v) snprintf(s->kind, sizeof(s->kind), "%s", v);
    v = json_string_field(root, "display_name"); if (v) snprintf(s->display_name, sizeof(s->display_name), "%s", v);
    v = json_string_field(root, "canonical_root"); if (v) snprintf(s->root, sizeof(s->root), "%s", v);
    v = json_string_field(root, "volume_identity"); if (v) snprintf(s->volume_identity, sizeof(s->volume_identity), "%s", v);
    v = json_string_field(root, "root_file_identity"); if (v) snprintf(s->root_file_identity, sizeof(s->root_file_identity), "%s", v);
    v = json_string_field(root, "policy_fingerprint"); if (v) snprintf(s->policy_fingerprint, sizeof(s->policy_fingerprint), "%s", v);
    jval *policy = root && root->t == J_OBJ ? json_get(root, "effective_policy") : NULL;
    jval *x = policy && policy->t == J_OBJ ? json_get(policy, "include_hidden") : NULL;
    s->include_hidden = x && x->t == J_BOOL && x->boolean;
    x = policy && policy->t == J_OBJ ? json_get(policy, "cross_filesystems") : NULL;
    s->cross_filesystems = x && x->t == J_BOOL && x->boolean;
    x = policy && policy->t == J_OBJ ? json_get(policy, "maximum_file_bytes") : NULL;
    s->max_file_bytes = x && x->t == J_NUM && x->num > 0 ? (unsigned long long)x->num : DEFAULT_MAX_FILE_BYTES;
    char state_real[PATH_MAX]; if (realpath(state_dir, state_real)) snprintf(s->state_dir, sizeof(s->state_dir), "%s", state_real);
    json_free(root); free(arena); free(raw);
    return s->root[0] && s->kind[0];
}

/* Compact SHA-256 used for file/content/chunk identities. */
typedef struct { uint32_t h[8]; uint64_t bits; unsigned char b[64]; size_t n; } Sha256;
static uint32_t srotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(Sha256 *c, const unsigned char *p) {
    static const uint32_t k[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba1,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c878fe,0x8cc70208,0x90befffa,0xa4506ce4,0xbef9a3f7,0xc67178f2};
    uint32_t w[64], a,b,d,e,f,g,h,t1,t2; int i; uint32_t cc;
    for (i=0;i<16;i++) w[i]=((uint32_t)p[4*i]<<24)|((uint32_t)p[4*i+1]<<16)|((uint32_t)p[4*i+2]<<8)|p[4*i+3];
    for (i=16;i<64;i++) { uint32_t x=w[i-15], y=w[i-2]; w[i]=w[i-16]+(srotr(x,7)^srotr(x,18)^(x>>3))+w[i-7]+(srotr(y,17)^srotr(y,19)^(y>>10)); }
    a=c->h[0]; cc=c->h[2]; d=c->h[3]; e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7]; b=c->h[1];
    for (i=0;i<64;i++) { uint32_t s1=srotr(e,6)^srotr(e,11)^srotr(e,25), ch=(e&f)^(~e&g), s0=srotr(a,2)^srotr(a,13)^srotr(a,22), maj=(a&b)^(a&cc)^(b&cc); t1=h+s1+ch+k[i]+w[i]; t2=s0+maj; h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2; }
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;
}
static void sha_init(Sha256 *c) { static const uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(c->h,h,sizeof(h));c->bits=0;c->n=0; }
static void sha_update(Sha256 *c, const void *data, size_t n) { const unsigned char *p=data; c->bits+=(uint64_t)n*8; while(n){size_t m=64-c->n;if(m>n)m=n;memcpy(c->b+c->n,p,m);c->n+=m;p+=m;n-=m;if(c->n==64){sha_block(c,c->b);c->n=0;}} }
static void sha_final(Sha256 *c, char out[65]) { size_t i; uint64_t bits=c->bits; c->b[c->n++]=0x80; while(c->n!=56){if(c->n==64){sha_block(c,c->b);c->n=0;}else c->b[c->n++]=0;} for(i=0;i<8;i++)c->b[56+i]=(unsigned char)(bits>>(56-8*i));sha_block(c,c->b);for(i=0;i<8;i++)snprintf(out+8*i,9,"%08x",c->h[i]);out[64]=0; }
static void sha_text(const void *p, size_t n, char out[65]) { Sha256 c; sha_init(&c); sha_update(&c,p,n); sha_final(&c,out); }

static int utf8_valid(const unsigned char *p, size_t n) {
    size_t i=0; while(i<n){unsigned char c=p[i];size_t k;if(c<0x80)k=1;else if(c>=0xc2&&c<=0xdf)k=2;else if(c>=0xe0&&c<=0xef)k=3;else if(c>=0xf0&&c<=0xf4)k=4;else return 0;if(i+k>n)return 0;for(size_t j=1;j<k;j++)if((p[i+j]&0xc0)!=0x80)return 0;if(k==3&&c==0xe0&&p[i+1]<0xa0)return 0;if(k==3&&c==0xed&&p[i+1]>=0xa0)return 0;if(k==4&&c==0xf0&&p[i+1]<0x90)return 0;if(k==4&&c==0xf4&&p[i+1]>=0x90)return 0;i+=k;}return 1;
}
static int text_candidate(const unsigned char *p, size_t n) {
    size_t i;
    if (!utf8_valid(p, n)) return 0;
    for (i = 0; i < n; ++i)
        if (p[i] == 0 || (p[i] < 0x09) || (p[i] > 0x0d && p[i] < 0x20)) return 0;
    return 1;
}
static size_t utf8_boundary_before(const unsigned char *p, size_t start, size_t at) { while(at>start && (p[at]&0xc0)==0x80) --at; return at; }

static const char *media_type_for(const unsigned char *p, size_t n) {
    if (n >= 5 && !memcmp(p, "%PDF-", 5)) return "application/pdf";
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && p[0]==0xff && p[1]==0xd8 && p[2]==0xff) return "image/jpeg";
    if (text_candidate(p, n)) return "text/plain";
    return "application/octet-stream";
}

static sqlite3_int64 mtime_ns(const struct stat *st) {
#if defined(__APPLE__)
    return (sqlite3_int64)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
    return (sqlite3_int64)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

static int read_regular_file(const char *path, unsigned long long max, unsigned char **out, size_t *out_n, struct stat *before) {
    int fd, flags=O_RDONLY; struct stat after; unsigned char *p; size_t off=0;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (lstat(path,before)!=0 || !S_ISREG(before->st_mode) || (unsigned long long)before->st_size>max) return 0;
    fd=open(path,flags); if(fd<0 || fstat(fd,&after)!=0 || after.st_dev!=before->st_dev || after.st_ino!=before->st_ino){if(fd>=0)close(fd);return 0;}
    p=malloc((size_t)before->st_size+1); if(!p){close(fd);return 0;}
    while(off<(size_t)before->st_size){ssize_t n=read(fd,p+off,(size_t)before->st_size-off);if(n<=0){free(p);close(fd);return 0;}off+=(size_t)n;}
    if(fstat(fd,&after)!=0 || after.st_size!=before->st_size || after.st_mtime!=before->st_mtime){free(p);close(fd);return 0;}
    close(fd);p[off]=0;*out=p;*out_n=off;return 1;
}

/* The gateway passes verified sidecar paths; this helper still keeps the
 * document parser out of the SQLite process's address space and caps the
 * response before buffering it. */
static char *run_capture_local(const char *program, char *const argv[], size_t limit, int *status) {
    int pipefd[2]; if (pipe(pipefd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    if (pid == 0) {
        close(pipefd[0]); dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
        execv(program, argv); _Exit(127);
    }
    close(pipefd[1]); char *out = malloc(limit + 1); size_t used = 0;
    if (!out) { close(pipefd[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return NULL; }
    while (used < limit) {
        ssize_t n = read(pipefd[0], out + used, limit - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(pipefd[0]);
    if (used == limit) { kill(pid, SIGKILL); waitpid(pid, status, 0); free(out); return NULL; }
    waitpid(pid, status, 0); out[used] = 0; return out;
}

static char *extract_evidence_text(const Scope *s, const char *path, const char *media) {
    if (!strcmp(media, "application/pdf")) {
        if (!s->extractor[0] || access(s->extractor, X_OK) != 0) return NULL;
        char *argv[] = {(char *)s->extractor, (char *)"--json", (char *)path, NULL};
        int status = 0; char *raw = run_capture_local(s->extractor, argv, 16 << 20, &status);
        if (!raw || !WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(raw); return NULL; }
        char *arena = NULL; jval *root = json_parse(raw, &arena);
        jval *ok = root && root->t == J_OBJ ? json_get(root, "ok") : NULL;
        jval *text = root && root->t == J_OBJ ? json_get(root, "text") : NULL;
        char *out = ok && ok->t == J_BOOL && ok->boolean && text && text->t == J_STR ? strdup(text->str) : NULL;
        json_free(root); free(arena); free(raw); return out;
    }
    if (!strcmp(media, "image/png") || !strcmp(media, "image/jpeg")) {
        if (!s->ocr[0] || access(s->ocr, X_OK) != 0) return NULL;
        char *argv[] = {(char *)s->ocr, (char *)"read", (char *)path, NULL};
        int status = 0; char *raw = run_capture_local(s->ocr, argv, 16 << 20, &status);
        if (!raw || !WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(raw); return NULL; }
        char *arena = NULL; jval *root = json_parse(raw, &arena);
        jval *lines = root && root->t == J_OBJ ? json_get(root, "lines") : NULL;
        StringBuffer out = {0};
        if (lines && lines->t == J_ARR) {
            for (int i = 0; i < lines->len; ++i) {
                jval *line = lines->kids[i], *value = line && line->t == J_OBJ ? json_get(line, "text") : NULL;
                if (value && value->t == J_STR) {
                    if (out.len && !sb_put(&out, "\n")) { free(out.data); out.data = NULL; break; }
                    if (!sb_put(&out, value->str)) { free(out.data); out.data = NULL; break; }
                }
            }
        }
        char *result = out.data && out.len ? out.data : NULL;
        if (!result) free(out.data);
        json_free(root); free(arena); free(raw); return result;
    }
    return NULL;
}

static int path_is_prefix(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(root, path, n) && (path[n] == 0 || path[n] == '/');
}

static void iso_now(char out[32]) {
    time_t now = time(NULL); struct tm tmv;
    gmtime_r(&now, &tmv); strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int scope_json_write(const Scope *s, const char *state, unsigned long long generation,
                            unsigned long long regular, unsigned long long indexed,
                            unsigned long long skipped, unsigned long long chunks,
                            unsigned long long source_bytes, unsigned long long text_bytes,
                            const char *active_db) {
    char path[PATH_MAX], now[32]; StringBuffer b = {0};
    if (!path_join_local(path, sizeof(path), s->scope_dir, "scope.json")) return 0;
    iso_now(now);
    if (!sb_put(&b, "{\"id\":") || !sb_json(&b, s->scope_id) || sb_put(&b, ",\"schema_version\":2,\"kind\":") == 0 ||
        !sb_json(&b, s->kind) || !sb_put(&b, ",\"display_name\":") || !sb_json(&b, s->display_name) ||
        !sb_put(&b, ",\"canonical_root\":") || !sb_json(&b, s->root) ||
        !sb_put(&b, ",\"volume_identity\":") || !sb_json(&b, s->volume_identity) ||
        !sb_put(&b, ",\"root_file_identity\":") || !sb_json(&b, s->root_file_identity) ||
        !sb_put(&b, ",\"policy_fingerprint\":") || !sb_json(&b, s->policy_fingerprint) ||
        !sb_put(&b, ",\"state\":") || !sb_json(&b, state) ||
        !sb_put(&b, ",\"freshness_state\":\"complete\",\"evidence_generation\":") ) { free(b.data); return 0; }
    char nums[512]; snprintf(nums, sizeof(nums), "%llu,\"enhancement_revision\":0,\"regular_files_seen\":%llu,\"files_indexed\":%llu,\"files_skipped\":%llu,\"chunks_indexed\":%llu,\"source_bytes_indexed\":%llu,\"extracted_text_bytes\":%llu,\"last_successful_build_at\":", generation, regular, indexed, skipped, chunks, source_bytes, text_bytes);
    if (!sb_put(&b, nums) || !sb_json(&b, now) || !sb_put(&b, ",\"last_successful_check_at\":") || !sb_json(&b, now) ||
        !sb_put(&b, ",\"active_database\":") || !sb_json(&b, active_db ? active_db : "") ||
        !sb_put(&b, ",\"effective_policy\":{\"include_hidden\":") || !sb_put(&b, s->include_hidden ? "true" : "false") ||
        !sb_put(&b, ",\"cross_filesystems\":") || !sb_put(&b, s->cross_filesystems ? "true" : "false") ||
        !sb_put(&b, ",\"maximum_file_bytes\":") ) { free(b.data); return 0; }
    snprintf(nums, sizeof(nums), "%llu,\"mandatory_exclusions\":[\".samosa\"],\"user_exclusions\":[]},\"warnings\":[]}\n", s->max_file_bytes);
    if (!sb_put(&b, nums)) { free(b.data); return 0; }
    int ok = atomic_text(path, b.data); free(b.data); return ok;
}

static int registry_write(const char *state_dir) {
    char scopes_dir[PATH_MAX], registry[PATH_MAX]; DIR *dir; struct dirent *de; StringBuffer b={0}; int first=1;
    if (!path_join_local(scopes_dir,sizeof(scopes_dir),state_dir,"scopes") || !path_join_local(registry,sizeof(registry),state_dir,"scopes.json")) return 0;
    dir=opendir(scopes_dir); if(!dir)return 0;
    if(!sb_put(&b,"{\"schema_version\":2,\"scopes\":[")){closedir(dir);return 0;}
    while((de=readdir(dir))){if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")||!valid_scope_id(de->d_name))continue; char p[PATH_MAX],*raw,*arena=NULL; jval *r; const char *root,*state; if(!path_join_local(p,sizeof(p),scopes_dir,de->d_name)||!path_join_local(p,sizeof(p),p,"scope.json"))continue;raw=read_all_local(p);if(!raw)continue;r=json_parse(raw,&arena);root=json_string_field(r,"canonical_root");state=json_string_field(r,"state");if(!first&&!sb_put(&b,",")){}first=0;sb_put(&b,"{\"id\":");sb_json(&b,de->d_name);sb_put(&b,",\"canonical_root\":");sb_json(&b,root?root:"");sb_put(&b,",\"state\":");sb_json(&b,state?state:"unbuilt");sb_put(&b,"}");json_free(r);free(arena);free(raw);}
    closedir(dir); if(!sb_put(&b,"]}\n")){free(b.data);return 0;} int ok=atomic_text(registry,b.data);free(b.data);return ok;
}

static int scope_event(const Scope *s, const char *kind, const char *state, unsigned long long generation, const char *message) {
    char path[PATH_MAX], now[32]; int fd; StringBuffer b={0};
    if(!path_join_local(path,sizeof(path),s->scope_dir,"events.jsonl"))return 0;
    iso_now(now);
    if(!sb_put(&b,"{\"seq\":") )return 0;
    /* Sequence allocation is serialized by the sidecar's advisory lock. */
    unsigned long long seq=1; FILE *f=fopen(path,"rb"); if(f){char line[4096];while(fgets(line,sizeof(line),f))if(strstr(line,"\"seq\":"))seq++;fclose(f);}
    char n[64];snprintf(n,sizeof(n),"%llu,\"time\":",seq);sb_put(&b,n);sb_json(&b,now);sb_put(&b,",\"job_id\":null,\"kind\":");sb_json(&b,kind);sb_put(&b,",\"state\":");sb_json(&b,state);snprintf(n,sizeof(n),",\"generation\":%llu,\"message\":",generation);sb_put(&b,n);sb_json(&b,message?message:"");sb_put(&b,"}\n");
    fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0600);if(fd<0){free(b.data);return 0;}size_t off=0;while(off<b.len){ssize_t nwrite=write(fd,b.data+off,b.len-off);if(nwrite<=0){close(fd);free(b.data);return 0;}off+=(size_t)nwrite;}int ok=fsync(fd)==0;close(fd);free(b.data);return ok;
}

static int scope_create(const char *state_dir, const char *id, const char *kind, const char *display, const char *root) {
    char canonical[PATH_MAX], scopes[PATH_MAX], dir[PATH_MAX], now[32]; struct stat st; Scope s={0};
    if(strcmp(kind,"folder") || !valid_scope_id(id) || !root || !realpath(root,canonical) || stat(canonical,&st)!=0 || !S_ISDIR(st.st_mode)) return 0;
    if(!path_join_local(scopes,sizeof(scopes),state_dir,"scopes") || !mkdir_parents(scopes) || !path_join_local(dir,sizeof(dir),scopes,id)) return 0;
    struct stat existing; if (stat(dir, &existing) == 0) return 0;
    if (!mkdir_one(dir)) return 0;
    if(!snprintf(s.state_dir,sizeof(s.state_dir),"%s",state_dir) || !snprintf(s.scope_dir,sizeof(s.scope_dir),"%s",dir) || !snprintf(s.scope_id,sizeof(s.scope_id),"%s",id)) return 0;
    snprintf(s.kind,sizeof(s.kind),"%s",kind);snprintf(s.display_name,sizeof(s.display_name),"%s",display?display:id);snprintf(s.root,sizeof(s.root),"%s",canonical);snprintf(s.volume_identity,sizeof(s.volume_identity),"%llu",(unsigned long long)st.st_dev);snprintf(s.root_file_identity,sizeof(s.root_file_identity),"%llu:%llu",(unsigned long long)st.st_dev,(unsigned long long)st.st_ino);s.max_file_bytes=DEFAULT_MAX_FILE_BYTES;sha_text("folder|false|false|268435456|.samosa",strlen("folder|false|false|268435456|.samosa"),s.policy_fingerprint);
    if(!scope_json_write(&s,"unbuilt",0,0,0,0,0,0,0,NULL)||!scope_event(&s,"chutni_build","queued",0,"scope created")||!registry_write(state_dir))return 0;
    iso_now(now); (void)now; return 1;
}

typedef struct { unsigned long long regular, skipped, indexed, chunks, source_bytes, text_bytes; } BuildCounts;

static int bind_file(sqlite3 *db, const char *file_id, const char *rel, const struct stat *st,
                     const char *hash, const char *media, const char *status, const char *reason,
                     int generation, const unsigned char *raw_path, size_t raw_len) {
    sqlite3_stmt *q=NULL; int ok;
    const char *sql="INSERT OR REPLACE INTO files(file_id,relative_path,stable_identity,path_bytes,display_path,size_bytes,mtime_ns,content_sha256,media_type,status,skip_reason,generation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
    if(sqlite3_prepare_v2(db,sql,-1,&q,NULL)!=SQLITE_OK)return 0;
    ok=bind_text(q,1,file_id)&&bind_text(q,2,rel)&&bind_text(q,3,"" )&&sqlite3_bind_blob(q,4,raw_path,(int)raw_len,SQLITE_TRANSIENT)==SQLITE_OK&&bind_text(q,5,rel)&&sqlite3_bind_int64(q,6,(sqlite3_int64)st->st_size)==SQLITE_OK&&sqlite3_bind_int64(q,7,mtime_ns(st))==SQLITE_OK&&bind_text(q,8,hash?hash:"")&&bind_text(q,9,media?media:"")&&bind_text(q,10,status?status:"")&&bind_text(q,11,reason?reason:"")&&sqlite3_bind_int(q,12,generation)==SQLITE_OK;
    if(ok)ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
    if(!ok)return 0;
    if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO manifest(file_id,path_bytes,stable_identity,size_bytes,mtime_ns,content_sha256,media_type,status,skip_reason,generation) VALUES(?,?,?,?,?,?,?,?,?,?)",-1,&q,NULL)!=SQLITE_OK)return 0;
    ok=bind_text(q,1,file_id)&&sqlite3_bind_blob(q,2,raw_path,(int)raw_len,SQLITE_TRANSIENT)==SQLITE_OK&&bind_text(q,3,"")&&sqlite3_bind_int64(q,4,st->st_size)==SQLITE_OK&&sqlite3_bind_int64(q,5,mtime_ns(st))==SQLITE_OK&&bind_text(q,6,hash?hash:"")&&bind_text(q,7,media?media:"")&&bind_text(q,8,status?status:"")&&bind_text(q,9,reason?reason:"")&&sqlite3_bind_int(q,10,generation)==SQLITE_OK;
    if(ok)ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;
}

static int inventory_dir(sqlite3 *db, const Scope *s, const char *abs, const char *rel, int generation, BuildCounts *counts) {
    DIR *d=opendir(abs); struct dirent *e; struct stat st; char child[PATH_MAX], childrel[PATH_MAX];
    if(build_canceled)return 1;
    if(!d)return 1;
    while(!build_canceled&&(e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;if(!rel[0]&&!strcmp(e->d_name,".samosa"))continue;if(!path_join_local(child,sizeof(child),abs,e->d_name))continue;int n=rel[0]?snprintf(childrel,sizeof(childrel),"%s/%s",rel,e->d_name):snprintf(childrel,sizeof(childrel),"%s",e->d_name);if(n<0||(size_t)n>=sizeof(childrel))continue;if(lstat(child,&st)!=0)continue;
        char fid[65];StringBuffer idbuf={0};sb_put(&idbuf,s->root);sb_put(&idbuf,"/");sb_put(&idbuf,childrel);sha_text(idbuf.data,idbuf.len,fid);free(idbuf.data);
        if(path_is_prefix(s->state_dir, child)) continue; /* Samosa-owned state is never indexed. */
        if(S_ISLNK(st.st_mode)){counts->skipped++;if(!bind_file(db,fid,childrel,&st,"","", "skipped","symlink",generation,(const unsigned char*)childrel,strlen(childrel))) {closedir(d);return 0;}continue;}
        if(e->d_name[0]=='.'&&!s->include_hidden){counts->skipped++;if(!bind_file(db,fid,childrel,&st,"","","skipped","hidden_excluded",generation,(const unsigned char*)childrel,strlen(childrel))){closedir(d);return 0;}continue;}
        if(S_ISDIR(st.st_mode)){if(!s->cross_filesystems&&st.st_dev!=(dev_t)strtoull(s->volume_identity,NULL,10)){counts->skipped++;bind_file(db,fid,childrel,&st,"","","skipped","cross_filesystem",generation,(const unsigned char*)childrel,strlen(childrel));continue;}if(!inventory_dir(db,s,child,childrel,generation,counts)){closedir(d);return 0;}continue;}
        counts->regular++;if(!S_ISREG(st.st_mode)){counts->skipped++;if(!bind_file(db,fid,childrel,&st,"","","skipped","not_regular_file",generation,(const unsigned char*)childrel,strlen(childrel))){closedir(d);return 0;}continue;}
        if((unsigned long long)st.st_size>s->max_file_bytes){counts->skipped++;if(!bind_file(db,fid,childrel,&st,"","","skipped","too_large",generation,(const unsigned char*)childrel,strlen(childrel))){closedir(d);return 0;}continue;}
        unsigned char *data=NULL;size_t len=0;struct stat opened;char hash[65];const char *media;int readable=read_regular_file(child,s->max_file_bytes,&data,&len,&opened);if(!readable){counts->skipped++;if(!bind_file(db,fid,childrel,&st,"","","skipped","changed_during_read",generation,(const unsigned char*)childrel,strlen(childrel))){closedir(d);return 0;}continue;}sha_text(data,len,hash);media=media_type_for(data,len);const char *status=!strcmp(media,"text/plain")||!strcmp(media,"application/pdf")||!strcmp(media,"image/png")||!strcmp(media,"image/jpeg")?"discovered":"skipped";const char *reason=!strcmp(status,"skipped")?"unsupported_type":"";if(!bind_file(db,fid,childrel,&opened,hash,media,status,reason,generation,(const unsigned char*)childrel,strlen(childrel))){free(data);closedir(d);return 0;}counts->source_bytes+=(unsigned long long)len;if(!strcmp(status,"skipped"))counts->skipped++;free(data);
    }
    closedir(d);return 1;
}

static int token_count_bounded(Tok *tok, const char *text, size_t len) {
    int ids[801];
    if (len > INT_MAX) return 801;
    return tok_encode_policy(tok, text, (int)len, ids, 801, 0);
}

static int insert_content(sqlite3 *db, const char *hash, const char *text, size_t len) {
    sqlite3_stmt *q=NULL; int ok=sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO contents(content_sha256,extraction_ref,parser_fingerprint,ocr_fingerprint,extracted_text,text_bytes) VALUES(?,?,?,?,?,?)",-1,&q,NULL)==SQLITE_OK;
    if(ok)ok=bind_text(q,1,hash)&&bind_text(q,2,hash)&&bind_text(q,3,"text-reader-v1")&&bind_text(q,4,"none")&&bind_text(q,5,text)&&sqlite3_bind_int64(q,6,(sqlite3_int64)len)==SQLITE_OK&&sqlite3_step(q)==SQLITE_DONE; if(!ok)fprintf(stderr,"chutni: content insert: %s\n",sqlite3_errmsg(db));sqlite3_finalize(q);return ok;
}

static int insert_chunk(sqlite3 *db, const char *chunk_id, const char *file_id, int ordinal, const char *rel,
                        const char *text, size_t start, size_t end, int tokens, int generation,
                        const char *tokenizer_fingerprint) {
    sqlite3_stmt *q=NULL; int ok=sqlite3_prepare_v2(db,"INSERT INTO chunks(chunk_id,file_id,ordinal,relative_path,title,page,section,start_offset,end_offset,text,token_count,tokenizer_fingerprint,generation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",-1,&q,NULL)==SQLITE_OK;
    if(ok)ok=bind_text(q,1,chunk_id)&&bind_text(q,2,file_id)&&sqlite3_bind_int(q,3,ordinal)==SQLITE_OK&&bind_text(q,4,rel)&&bind_text(q,5,"")&&sqlite3_bind_null(q,6)==SQLITE_OK&&sqlite3_bind_null(q,7)==SQLITE_OK&&sqlite3_bind_int64(q,8,(sqlite3_int64)start)==SQLITE_OK&&sqlite3_bind_int64(q,9,(sqlite3_int64)end)==SQLITE_OK&&bind_text(q,10,text)&&sqlite3_bind_int(q,11,tokens)==SQLITE_OK&&bind_text(q,12,tokenizer_fingerprint)&&sqlite3_bind_int(q,13,generation)==SQLITE_OK&&sqlite3_step(q)==SQLITE_DONE; if(!ok)fprintf(stderr,"chutni: chunk insert: %s\n",sqlite3_errmsg(db));sqlite3_finalize(q);if(!ok)return 0;
    if(sqlite3_prepare_v2(db,"INSERT INTO chunks_fts(rowid,chunk_id,relative_path,title,text) SELECT rowid,chunk_id,relative_path,title,text FROM chunks WHERE chunk_id=?",-1,&q,NULL)!=SQLITE_OK){fprintf(stderr,"chutni: fts prepare: %s\n",sqlite3_errmsg(db));return 0;}ok=bind_text(q,1,chunk_id)&&sqlite3_step(q)==SQLITE_DONE;if(!ok)fprintf(stderr,"chutni: fts insert: %s\n",sqlite3_errmsg(db));sqlite3_finalize(q);return ok;
}

static size_t next_chunk_end(Tok *tok, const unsigned char *p, size_t start, size_t len) {
    size_t remaining=len-start, lo=start+1, hi=len, best=start;
    if(token_count_bounded(tok,(const char*)p+start,remaining)<=800)return len;
    while(lo<=hi){size_t mid=lo+(hi-lo)/2;mid=utf8_boundary_before(p,start,mid);if(mid<=start){lo=start+1;continue;}int n=token_count_bounded(tok,(const char*)p+start,mid-start);if(n<=800){best=mid;lo=mid+1;}else hi=mid-1;}
    if(best==start){size_t b=start+1;while(b<len&&(p[b]&0xc0)==0x80)b++;return b;}
    /* Prefer a complete paragraph/line when it remains within the token cap. */
    size_t cut=best;while(cut>start&&p[cut-1]!='\n'&&p[cut-1]!='\r')cut--;if(cut>start&&token_count_bounded(tok,(const char*)p+start,cut-start)<=800)return cut;return best;
}

static int build_text_chunks(sqlite3 *db, const Scope *s, Tok *tok, int generation, BuildCounts *counts, const char *tokenizer_fingerprint) {
    sqlite3_stmt *q=NULL;int rc=sqlite3_prepare_v2(db,"SELECT m.file_id,f.relative_path,m.content_sha256,m.media_type FROM manifest m JOIN files f ON f.file_id=m.file_id WHERE m.generation=? AND m.status='discovered' AND m.media_type IN ('text/plain','application/pdf','image/png','image/jpeg') ORDER BY f.relative_path,m.file_id",-1,&q,NULL);if(rc!=SQLITE_OK){fprintf(stderr,"chutni: manifest query: %s\n",sqlite3_errmsg(db));return 0;}sqlite3_bind_int(q,1,generation);
    while((rc=sqlite3_step(q))==SQLITE_ROW){if(build_canceled){sqlite3_finalize(q);return 1;}const char *fid=(const char*)sqlite3_column_text(q,0),*rel=(const char*)sqlite3_column_text(q,1),*hash=(const char*)sqlite3_column_text(q,2),*media=(const char*)sqlite3_column_text(q,3);char path[PATH_MAX];if(!path_join_local(path,sizeof(path),s->root,rel)){continue;}unsigned char *data=NULL;size_t len=0;struct stat st;if(!read_regular_file(path,s->max_file_bytes,&data,&len,&st)){free(data);continue;}char *evidence=NULL;if(!strcmp(media,"text/plain")){if(utf8_valid(data,len)){evidence=strdup((const char*)data);}free(data);}else{free(data);evidence=extract_evidence_text(s,path,media);}if(!evidence)continue;len=strlen(evidence);if(!utf8_valid((const unsigned char*)evidence,len)){free(evidence);continue;}if(!insert_content(db,hash,evidence,len)){free(evidence);sqlite3_finalize(q);return 0;}const unsigned char *bytes=(const unsigned char*)evidence;size_t off=0;int ord=0;while(off<len){size_t end=next_chunk_end(tok,bytes,off,len);int n=token_count_bounded(tok,(const char*)bytes+off,end-off);if(n>800||end<=off){free(evidence);sqlite3_finalize(q);return 0;}char key[256],cid[65];snprintf(key,sizeof(key),"%s/%s/%s/%zu/%zu",hash,fid,CHUNKER_FINGERPRINT,off,end);sha_text(key,strlen(key),cid);char *chunk=malloc(end-off+1);if(!chunk){free(evidence);sqlite3_finalize(q);return 0;}memcpy(chunk,bytes+off,end-off);chunk[end-off]=0;if(!insert_chunk(db,cid,fid,ord++,rel,chunk,off,end,n,generation,tokenizer_fingerprint)){free(chunk);free(evidence);sqlite3_finalize(q);return 0;}free(chunk);counts->chunks++;off=end;}counts->indexed++;counts->text_bytes+=(unsigned long long)len;free(evidence);}
    sqlite3_finalize(q);if(rc!=SQLITE_DONE)fprintf(stderr,"chutni: manifest step: %s (%d)\n",sqlite3_errmsg(db),rc);return rc==SQLITE_DONE;
}

static int validate_staging(sqlite3 *db) {
    sqlite3_stmt *q=NULL;int ok=sqlite3_prepare_v2(db,"PRAGMA quick_check",-1,&q,NULL)==SQLITE_OK&&sqlite3_step(q)==SQLITE_ROW&&!strcmp((const char*)sqlite3_column_text(q,0),"ok");sqlite3_finalize(q);if(!ok)return 0;if(!exec_sql(db,"INSERT INTO chunks_fts(chunks_fts) VALUES('integrity-check');"))return 0;if(!exec_sql(db,"PRAGMA foreign_key_check;"))return 0;int a=0,b=0;return sqlite3_wal_checkpoint_v2(db,NULL,SQLITE_CHECKPOINT_PASSIVE,&a,&b)==SQLITE_OK;
}

static unsigned long long active_generation(const Scope *s) {
    char path[PATH_MAX],*raw,*arena=NULL; jval *r; jval *v;unsigned long long n=0;if(!path_join_local(path,sizeof(path),s->scope_dir,"active.json"))return 0;raw=read_all_local(path);if(!raw)return 0;r=json_parse(raw,&arena);v=r&&r->t==J_OBJ?json_get(r,"evidence_generation"):NULL;if(v&&v->t==J_NUM&&v->num>=0)n=(unsigned long long)v->num;json_free(r);free(arena);free(raw);return n;
}

static int insert_scope_metadata(sqlite3 *db, const Scope *s, unsigned long long generation,
                                 const char *db_name, const BuildCounts *c) {
    sqlite3_stmt *q=NULL; char policy[256]; char now[32]; iso_now(now);
    snprintf(policy,sizeof(policy),"{\"include_hidden\":%s,\"cross_filesystems\":%s,\"maximum_file_bytes\":%llu,\"mandatory_exclusions\":[\".samosa\"],\"user_exclusions\":[]}",s->include_hidden?"true":"false",s->cross_filesystems?"true":"false",s->max_file_bytes);
    int ok=sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO scope_meta(scope_id,schema_version,kind,display_name,canonical_root,volume_identity,root_file_identity,policy_json,policy_fingerprint,state,freshness_state,evidence_generation,enhancement_revision,current_job_id,last_successful_build_at,last_successful_check_at,regular_files_seen,files_indexed,files_skipped,chunks_indexed,source_bytes_indexed,extracted_text_bytes) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",-1,&q,NULL)==SQLITE_OK;
    if(ok)ok=bind_text(q,1,s->scope_id)&&sqlite3_bind_int(q,2,2)==SQLITE_OK&&bind_text(q,3,s->kind)&&bind_text(q,4,s->display_name)&&bind_text(q,5,s->root)&&bind_text(q,6,s->volume_identity)&&bind_text(q,7,s->root_file_identity)&&bind_text(q,8,policy)&&bind_text(q,9,s->policy_fingerprint)&&bind_text(q,10,"ready")&&bind_text(q,11,"complete")&&sqlite3_bind_int64(q,12,(sqlite3_int64)generation)==SQLITE_OK&&sqlite3_bind_int(q,13,0)==SQLITE_OK&&sqlite3_bind_null(q,14)==SQLITE_OK&&bind_text(q,15,now)&&bind_text(q,16,now)&&sqlite3_bind_int64(q,17,c->regular)==SQLITE_OK&&sqlite3_bind_int64(q,18,c->indexed)==SQLITE_OK&&sqlite3_bind_int64(q,19,c->skipped)==SQLITE_OK&&sqlite3_bind_int64(q,20,c->chunks)==SQLITE_OK&&sqlite3_bind_int64(q,21,c->source_bytes)==SQLITE_OK&&sqlite3_bind_int64(q,22,c->text_bytes)==SQLITE_OK&&sqlite3_step(q)==SQLITE_DONE;
    sqlite3_finalize(q);if(!ok)return 0;
    if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO publications(generation,state,created_at,committed_at,database_name) VALUES(?,?,?,?,?)",-1,&q,NULL)!=SQLITE_OK)return 0;
    ok=sqlite3_bind_int64(q,1,(sqlite3_int64)generation)==SQLITE_OK&&bind_text(q,2,"validated")&&bind_text(q,3,now)&&sqlite3_bind_null(q,4)==SQLITE_OK&&bind_text(q,5,db_name)&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;
}

static int publish_staging(const Scope *s, const char *db_name, unsigned long long generation, const BuildCounts *c) {
    char path[PATH_MAX];StringBuffer b={0};char n[256];if(!path_join_local(path,sizeof(path),s->scope_dir,"active.json"))return 0;snprintf(n,sizeof(n),"{\"schema_version\":2,\"database\":");sb_put(&b,n);sb_json(&b,db_name);snprintf(n,sizeof(n),",\"evidence_generation\":%llu,\"enhancement_revision\":0,\"published_at\":",generation);sb_put(&b,n);char now[32];iso_now(now);sb_json(&b,now);sb_put(&b,"}\n");int ok=atomic_text(path,b.data);free(b.data);if(!ok)return 0;ok=scope_json_write(s,"ready",generation,c->regular,c->indexed,c->skipped,c->chunks,c->source_bytes,c->text_bytes,db_name);if(ok)ok=scope_event(s,"chutni_build","completed",generation,"evidence published");if(ok)ok=registry_write(s->state_dir);return ok;
}

static int scope_build(const char *state_dir, const char *id, const char *tokenizer,
                       const char *extractor, const char *ocr, int inventory_only) {
    Scope s;if(!load_scope(state_dir,id,&s))return 0;
    if (extractor) snprintf(s.extractor, sizeof(s.extractor), "%s", extractor);
    if (ocr) snprintf(s.ocr, sizeof(s.ocr), "%s", ocr);
    struct stat rootst;if(stat(s.root,&rootst)!=0||strcmp(s.volume_identity,"" )==0||strtoull(s.volume_identity,NULL,10)!=(unsigned long long)rootst.st_dev){scope_json_write(&s,"disconnected",active_generation(&s),0,0,0,0,0,0,NULL);scope_event(&s,"chutni_build","failed",0,"volume identity changed");return 0;}
    if(!inventory_only&&(!tokenizer||!*tokenizer))return 0;
    if(!inventory_only&&access(tokenizer,R_OK)!=0)return 0;
    unsigned long long generation=active_generation(&s)+1;char dbname[128],dbpath[PATH_MAX];snprintf(dbname,sizeof(dbname),"index.g%llu.sqlite3",generation);if(!path_join_local(dbpath,sizeof(dbpath),s.scope_dir,dbname))return 0;unlink(dbpath);unlink(strcat(strcpy((char[PATH_MAX]){0},dbpath),"-wal"));unlink(strcat(strcpy((char[PATH_MAX]){0},dbpath),"-shm"));
    sqlite3 *db=open_db(dbpath,1);if(!db)return 0;BuildCounts c={0};build_canceled=0;
    if(!exec_sql(db,"BEGIN IMMEDIATE;")||!inventory_dir(db,&s,s.root,"",(int)generation,&c)){fprintf(stderr,"chutni: inventory failed\n");sqlite3_close(db);unlink(dbpath);return 0;}
    if(!inventory_only){char *tok_bytes=read_all_local(tokenizer);char tok_fp[65],combined_fp[65],fp_input[160];if(!tok_bytes){exec_sql(db,"ROLLBACK;");sqlite3_close(db);unlink(dbpath);return 0;}sha_text(tok_bytes,strlen(tok_bytes),tok_fp);free(tok_bytes);snprintf(fp_input,sizeof(fp_input),"%s/%s",tok_fp,CHUNKER_FINGERPRINT);sha_text(fp_input,strlen(fp_input),combined_fp);Tok tok;tok_load(&tok,tokenizer);if(!build_text_chunks(db,&s,&tok,(int)generation,&c,combined_fp)){fprintf(stderr,"chutni: chunking failed\n");tok_free(&tok);exec_sql(db,"ROLLBACK;");sqlite3_close(db);unlink(dbpath);return 0;}tok_free(&tok);}
    if(build_canceled){exec_sql(db,"ROLLBACK;");sqlite3_close(db);unlink(dbpath);scope_event(&s,"chutni_build","canceled",generation,"build canceled before publication");return 2;}
    c.skipped = c.regular >= c.indexed ? c.regular - c.indexed : 0;
    if (!insert_scope_metadata(db, &s, generation, dbname, &c)) { fprintf(stderr,"chutni: scope metadata failed\n"); exec_sql(db,"ROLLBACK;"); sqlite3_close(db); unlink(dbpath); return 0; }
    if(!exec_sql(db,"COMMIT;")||!validate_staging(db)){fprintf(stderr,"chutni: staging validation failed\n");sqlite3_close(db);unlink(dbpath);return 0;}
    int frames=0,checkpointed=0;sqlite3_wal_checkpoint_v2(db,NULL,SQLITE_CHECKPOINT_TRUNCATE,&frames,&checkpointed);sqlite3_close(db);int fd=open(dbpath,O_RDONLY);if(fd>=0){int ok=fsync(fd)==0;close(fd);if(!ok){unlink(dbpath);return 0;}}if(!publish_staging(&s,dbname,generation,&c)){fprintf(stderr,"chutni: publication failed\n");unlink(dbpath);return 0;}return 1;
}

static int scope_show(const char *state_dir, const char *id) {Scope s;if(!load_scope(state_dir,id,&s))return 0;char path[PATH_MAX],*raw;if(!path_join_local(path,sizeof(path),s.scope_dir,"scope.json"))return 0;raw=read_all_local(path);if(!raw)return 0;fputs(raw,stdout);free(raw);return 1;}

static int scope_query(const char *state_dir, const char *id, const char *query) {Scope s;if(!load_scope(state_dir,id,&s))return 0;char path[PATH_MAX],*raw,*arena=NULL; jval *r;const char *dbn;if(!path_join_local(path,sizeof(path),s.scope_dir,"active.json")||(raw=read_all_local(path))==NULL)return 0;r=json_parse(raw,&arena);dbn=json_string_field(r,"database");char dbpath[PATH_MAX];int ok=dbn&&path_join_local(dbpath,sizeof(dbpath),s.scope_dir,dbn);json_free(r);free(arena);free(raw);if(!ok)return 0;sqlite3 *db=open_db(dbpath,0);if(!db)return 0;ok=query_db(db,query,20);sqlite3_close(db);return ok;}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "scope-create")) {
        if (argc < 7 || !scope_create(argv[2], argv[3], argv[4], argv[5], argv[6])) return 2;
        puts("{\"ok\":true,\"state\":\"unbuilt\"}"); return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "scope-show")) return scope_show(argv[2], argv[3]) ? 0 : 2;
    if (argc >= 4 && !strcmp(argv[1], "scope-inventory")) {
        signal(SIGINT,on_build_signal); signal(SIGTERM,on_build_signal);
        int rc=scope_build(argv[2],argv[3],NULL,NULL,NULL,1); return rc==1?0:(rc==2?2:1);
    }
    if (argc >= 5 && !strcmp(argv[1], "scope-build")) {
        signal(SIGINT,on_build_signal); signal(SIGTERM,on_build_signal);
        int rc=scope_build(argv[2],argv[3],argv[4],argc >= 6 ? argv[5] : NULL,
                           argc >= 7 ? argv[6] : NULL,0); return rc==1?0:(rc==2?2:1);
    }
    if (argc >= 5 && !strcmp(argv[1], "scope-query")) return scope_query(argv[2],argv[3],argv[4]) ? 0 : 2;
    if (argc < 3) { fprintf(stderr, "usage: samosa-chutni-db init|add-file|add-chunk|query|integrity DB ...\n       samosa-chutni-db scope-create STATE_DIR ID folder DISPLAY_NAME ROOT\n       samosa-chutni-db scope-inventory STATE_DIR ID\n       samosa-chutni-db scope-build STATE_DIR ID TOKENIZER\n       samosa-chutni-db scope-show|scope-query STATE_DIR ID [QUERY]\n"); return 2; }
    int create = !strcmp(argv[1], "init") || !strcmp(argv[1], "add-file") || !strcmp(argv[1], "add-chunk");
    sqlite3 *db = open_db(argv[2], create); if (!db) return 1;
    int ok = 0;
    if (!strcmp(argv[1], "init")) ok = 1;
    else if (!strcmp(argv[1], "add-file")) ok = add_file(db, argc - 1, argv + 1);
    else if (!strcmp(argv[1], "add-chunk")) ok = add_chunk(db, argc - 1, argv + 1);
    else if (!strcmp(argv[1], "query") && argc == 4) ok = query_db(db, argv[3], 20);
    else if (!strcmp(argv[1], "integrity") && argc == 3) ok = integrity_db(db);
    sqlite3_close(db); return ok ? 0 : 2;
}
