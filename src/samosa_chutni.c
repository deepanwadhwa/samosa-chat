#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sqlite/sqlite3.h"
#include "json.h"
#include "tok.h"


#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_FILE_SIZE (25 * 1024 * 1024)

#define EX_OK 0
#define EX_USAGE 1
#define EX_ERROR 2

const char* SCHEMA = 
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT"
    ");"
    "INSERT OR IGNORE INTO meta (key, value) VALUES ('schema_version', '2');"
    "CREATE TABLE IF NOT EXISTS roots ("
    "  root_id TEXT PRIMARY KEY,"
    "  path TEXT UNIQUE NOT NULL,"
    "  volume_identity TEXT,"
    "  root_file_identity TEXT,"
    "  state TEXT DEFAULT 'connected'"
    ");"
    "CREATE TABLE IF NOT EXISTS files ("
    "  id INTEGER PRIMARY KEY,"
    "  root_id TEXT,"
    "  rel_path TEXT,"
    "  size INTEGER,"
    "  mtime INTEGER,"
    "  status TEXT,"
    "  skip_reason TEXT,"
    "  UNIQUE(root_id, rel_path)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_files_path ON files(root_id, rel_path);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5("
    "  content,"
    "  file_id UNINDEXED,"
    "  chunk_index UNINDEXED"
    ");";

void print_json_error(const char* msg, const char* detail) {
    printf("{\"status\": \"error\", \"message\": \"%s\", \"detail\": \"%s\"}\n", msg, detail ? detail : "");
}


static hmap T;
static int tok_initialized = 0;

void ensure_tokenizer() {
    if (!tok_initialized) {
        // Mock tokenization for MVP if tokenizer.json is missing
        // In a real implementation, we'd call tok_load(&T, "tokenizer.json");
        tok_initialized = 1;
    }
}

int init_db(const char* db_path) {
    sqlite3* db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        print_json_error("Failed to open database", sqlite3_errmsg(db));
        return EX_ERROR;
    }
    char* err_msg = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err_msg) != SQLITE_OK) {
        print_json_error("Failed to create schema", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return EX_ERROR;
    }
    sqlite3_close(db);
    printf("{\"status\": \"ok\", \"db\": \"%s\"}\n", db_path);
    return EX_OK;
}



void process_file(sqlite3* db, const char* root_id, const char* rel_path, const char* full_path, sqlite3_int64 file_id) {
    char cmd[PATH_MAX + 100];
    snprintf(cmd, sizeof(cmd), "./build/samosa-extract --json \"%s\"", full_path);
    FILE* fp = popen(cmd, "r");
    if (!fp) return;
    
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    pclose(fp);
    
    char* text_start = strstr(buf, "\"text\":\"");
    if (text_start) {
        text_start += 8;
        char* text_end = strchr(text_start, '\"');
        if (text_end) {
            *text_end = '\0';
            
            sqlite3_stmt* stmt;
            const char* sql = "INSERT INTO chunks_fts(content, file_id, chunk_index) VALUES (?, ?, 0)";
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, text_start, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, file_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}


int should_exclude_path(const char* path) {
    const char* excludes[] = {
        "/System",
        "/private",
        "/dev",
        "/bin",
        "/sbin",
        "/usr",
        "/Library",
        "/.ssh",
        "/.gnupg",
        "/.samosa",
        "/.gemini",
        "/.Trash",
        "/.git",
        NULL
    };
    for (int i = 0; excludes[i]; i++) {
        if (strstr(path, excludes[i])) return 1;
    }
    return 0;
}

void walk_directory(sqlite3* db, const char* root_id, const char* base_path, const char* rel_path, dev_t root_dev, int* files_scanned) {
    if (*files_scanned >= 50000) return; // Quota limit

    char current_dir[PATH_MAX];
    if (rel_path[0] == '\0') snprintf(current_dir, sizeof(current_dir), "%s", base_path);
    else snprintf(current_dir, sizeof(current_dir), "%s/%s", base_path, rel_path);

    if (should_exclude_path(current_dir)) return;

    DIR* dir = opendir(current_dir);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (*files_scanned >= 50000) break; // Quota limit
        if (entry->d_name[0] == '.') continue; // Skip hidden, ., ..

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, entry->d_name);
        
        if (should_exclude_path(full_path)) continue;

        char next_rel_path[PATH_MAX];
        if (rel_path[0] == '\0') snprintf(next_rel_path, sizeof(next_rel_path), "%s", entry->d_name);
        else snprintf(next_rel_path, sizeof(next_rel_path), "%s/%s", rel_path, entry->d_name);

        struct stat st;
        if (lstat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev == root_dev) {
                walk_directory(db, root_id, base_path, next_rel_path, root_dev, files_scanned);
            }
        } else if (S_ISREG(st.st_mode)) {
            (*files_scanned)++;
            const char* skip = NULL;
            if (st.st_size > MAX_FILE_SIZE) skip = "oversized";

            sqlite3_stmt* chk_stmt;
            sqlite3_prepare_v2(db, "SELECT id, size, mtime FROM files WHERE root_id=? AND rel_path=?", -1, &chk_stmt, NULL);
            sqlite3_bind_text(chk_stmt, 1, root_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(chk_stmt, 2, next_rel_path, -1, SQLITE_STATIC);
            
            int exists = 0;
            int changed = 1;
            sqlite3_int64 file_id = -1;
            
            if (sqlite3_step(chk_stmt) == SQLITE_ROW) {
                exists = 1;
                file_id = sqlite3_column_int64(chk_stmt, 0);
                sqlite3_int64 old_size = sqlite3_column_int64(chk_stmt, 1);
                sqlite3_int64 old_mtime = sqlite3_column_int64(chk_stmt, 2);
                if (old_size == st.st_size && old_mtime == st.st_mtime) {
                    changed = 0;
                }
            }
            sqlite3_finalize(chk_stmt);

            if (changed) {
                if (exists) {
                    sqlite3_stmt* del_stmt;
                    sqlite3_prepare_v2(db, "DELETE FROM chunks_fts WHERE file_id=?", -1, &del_stmt, NULL);
                    sqlite3_bind_int64(del_stmt, 1, file_id);
                    sqlite3_step(del_stmt);
                    sqlite3_finalize(del_stmt);
                }
                
                sqlite3_stmt* ins_stmt;
                const char* ins_sql = "INSERT OR REPLACE INTO files (id, root_id, rel_path, size, mtime, status, skip_reason) VALUES (?, ?, ?, ?, ?, ?, ?)";
                sqlite3_prepare_v2(db, ins_sql, -1, &ins_stmt, NULL);
                if (exists) sqlite3_bind_int64(ins_stmt, 1, file_id);
                else sqlite3_bind_null(ins_stmt, 1);
                sqlite3_bind_text(ins_stmt, 2, root_id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins_stmt, 3, next_rel_path, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ins_stmt, 4, st.st_size);
                sqlite3_bind_int64(ins_stmt, 5, st.st_mtime);
                sqlite3_bind_text(ins_stmt, 6, skip ? "skipped" : "indexed", -1, SQLITE_STATIC);
                if (skip) sqlite3_bind_text(ins_stmt, 7, skip, -1, SQLITE_STATIC);
                else sqlite3_bind_null(ins_stmt, 7);
                sqlite3_step(ins_stmt);
                file_id = sqlite3_last_insert_rowid(db);
                sqlite3_finalize(ins_stmt);
                
                if (!skip) {
                    process_file(db, root_id, next_rel_path, full_path, file_id);
                }
            } else {
                sqlite3_stmt* upd_stmt;
                sqlite3_prepare_v2(db, "UPDATE files SET status='indexed' WHERE id=?", -1, &upd_stmt, NULL);
                sqlite3_bind_int64(upd_stmt, 1, file_id);
                sqlite3_step(upd_stmt);
                sqlite3_finalize(upd_stmt);
            }
        }
    }
    closedir(dir);
}



int query_chutni(sqlite3* db, const char* root_id, const char* path_prefix, const char* search_query) {
    sqlite3_stmt* stmt;
    const char* sql = "SELECT chunks_fts.content, files.rel_path FROM chunks_fts "
                      "JOIN files ON chunks_fts.file_id = files.id "
                      "WHERE chunks_fts MATCH ? AND files.root_id = ? AND (files.rel_path LIKE ? OR files.rel_path = ?) "
                      "LIMIT 10;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        print_json_error("Failed to prepare query", sqlite3_errmsg(db));
        return EX_ERROR;
    }
    
    char like_pattern[PATH_MAX];
    if (!path_prefix || path_prefix[0] == '\0' || (path_prefix[0] == '/' && path_prefix[1] == '\0')) {
        snprintf(like_pattern, sizeof(like_pattern), "%%");
    } else {
        const char* prefix = path_prefix;
        if (prefix[0] == '/') prefix++; // strip leading slash
        snprintf(like_pattern, sizeof(like_pattern), "%s/%%", prefix);
    }
    
    // We bind exact path_prefix (without leading slash) as the fourth param
    const char* exact_match = path_prefix;
    if (exact_match && exact_match[0] == '/') exact_match++;
    
    // FTS5 MATCH pattern (we can just pass search_query as is for now)
    sqlite3_bind_text(stmt, 1, search_query, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, root_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, like_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, exact_match ? exact_match : "", -1, SQLITE_STATIC);
    
    printf("{\"status\": \"ok\", \"results\": [\n");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* content = sqlite3_column_text(stmt, 0);
        const unsigned char* rel_path = sqlite3_column_text(stmt, 1);
        
        if (!first) printf(",\n");
        // We'd properly JSON escape content in production, here we use a simple representation for MVP
        // since we just need the system to flow.
        printf("  {\"path\": \"%s\", \"content\": \"...", rel_path);
        // Print safely up to 100 chars
        for (int i = 0; i < 100 && content[i]; i++) {
            if (content[i] == '\n' || content[i] == '\r' || content[i] == '\"') printf(" ");
            else printf("%c", content[i]);
        }
        printf("...\" }");
        first = 0;
    }
    printf("\n]}\n");
    sqlite3_finalize(stmt);
    return EX_OK;
}

int forget_root(sqlite3* db, const char* root_id) {
    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM chunks_fts WHERE file_id IN (SELECT id FROM files WHERE root_id=?)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    sqlite3_prepare_v2(db, "DELETE FROM files WHERE root_id=?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    sqlite3_prepare_v2(db, "DELETE FROM roots WHERE root_id=?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    
    printf("{\"status\": \"ok\", \"action\": \"forget\", \"root_id\": \"%s\"}\n", root_id);
    return EX_OK;
}


int scan_directory(sqlite3* db, const char* root_id, const char* base_path) {
    struct stat st;
    if (stat(base_path, &st) != 0) {
        print_json_error("Cannot access root directory", base_path);
        return EX_ERROR;
    }
    
    char dev_str[64], ino_str[64];
    snprintf(dev_str, sizeof(dev_str), "%llu", (unsigned long long)st.st_dev);
    snprintf(ino_str, sizeof(ino_str), "%llu", (unsigned long long)st.st_ino);

    // Check existing identity
    sqlite3_stmt* chk_root;
    sqlite3_prepare_v2(db, "SELECT volume_identity, root_file_identity FROM roots WHERE root_id=?", -1, &chk_root, NULL);
    sqlite3_bind_text(chk_root, 1, root_id, -1, SQLITE_STATIC);
    if (sqlite3_step(chk_root) == SQLITE_ROW) {
        const char* old_dev = (const char*)sqlite3_column_text(chk_root, 0);
        const char* old_ino = (const char*)sqlite3_column_text(chk_root, 1);
        if (strcmp(old_dev, dev_str) != 0 || strcmp(old_ino, ino_str) != 0) {
            sqlite3_finalize(chk_root);
            print_json_error("Volume identity mismatch (drive changed or disconnected)", base_path);
            return EX_ERROR;
        }
    }
    sqlite3_finalize(chk_root);

    // Upsert root
    sqlite3_stmt* root_stmt;
    const char* root_sql = "INSERT INTO roots (root_id, path, volume_identity, root_file_identity) VALUES (?, ?, ?, ?) "
                           "ON CONFLICT(root_id) DO UPDATE SET path=excluded.path, volume_identity=excluded.volume_identity, root_file_identity=excluded.root_file_identity;";
    sqlite3_prepare_v2(db, root_sql, -1, &root_stmt, NULL);
    sqlite3_bind_text(root_stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(root_stmt, 2, base_path, -1, SQLITE_STATIC);
    sqlite3_bind_text(root_stmt, 3, dev_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(root_stmt, 4, ino_str, -1, SQLITE_STATIC);
    sqlite3_step(root_stmt);
    sqlite3_finalize(root_stmt);

    // Begin Transaction
    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    // Mark all existing files as unvisited
    sqlite3_stmt* unv_stmt;
    sqlite3_prepare_v2(db, "UPDATE files SET status='unvisited' WHERE root_id=?", -1, &unv_stmt, NULL);
    sqlite3_bind_text(unv_stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_step(unv_stmt);
    sqlite3_finalize(unv_stmt);

    int files_scanned = 0;
    walk_directory(db, root_id, base_path, "", st.st_dev, &files_scanned);
    
    // Delete chunks for files that are still unvisited (i.e. deleted from disk)
    sqlite3_exec(db, "DELETE FROM chunks_fts WHERE file_id IN (SELECT id FROM files WHERE status='unvisited')", NULL, NULL, NULL);
    // Delete files that are unvisited
    sqlite3_exec(db, "DELETE FROM files WHERE status='unvisited'", NULL, NULL, NULL);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    
    // Count files to return telemetry
    sqlite3_stmt* count_stmt;
    int files_seen = 0;
    int files_skipped = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE root_id = ?;", -1, &count_stmt, NULL);
    sqlite3_bind_text(count_stmt, 1, root_id, -1, SQLITE_STATIC);
    if (sqlite3_step(count_stmt) == SQLITE_ROW) files_seen = sqlite3_column_int(count_stmt, 0);
    sqlite3_finalize(count_stmt);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE root_id = ? AND status = 'skipped';", -1, &count_stmt, NULL);
    sqlite3_bind_text(count_stmt, 1, root_id, -1, SQLITE_STATIC);
    if (sqlite3_step(count_stmt) == SQLITE_ROW) files_skipped = sqlite3_column_int(count_stmt, 0);
    sqlite3_finalize(count_stmt);

    if (files_scanned >= 50000) {
        printf("{\"status\": \"quota_exceeded\", \"action\": \"scan\", \"root_id\": \"%s\", \"files_seen\": %d, \"files_skipped\": %d}\n", root_id, files_seen, files_skipped);
    } else {
        printf("{\"status\": \"ok\", \"action\": \"scan\", \"root_id\": \"%s\", \"files_seen\": %d, \"files_skipped\": %d}\n", root_id, files_seen, files_skipped);
    }
    return EX_OK;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_json_error("Usage: samosa-chutni <command> <db_path>", NULL);
        return EX_USAGE;
    }
    
    const char* cmd = argv[1];
    const char* db_path = argv[2];
    
    if (strcmp(cmd, "init") == 0) {
        return init_db(db_path);
    } else if (strcmp(cmd, "scan") == 0 || strcmp(cmd, "refresh") == 0) {
        if (argc < 5) {
            print_json_error("Usage: samosa-chutni scan|refresh <db_path> <root_id> <root_path>", NULL);
            return EX_USAGE;
        }
        sqlite3* db;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) return EX_ERROR;
        int res = scan_directory(db, argv[3], argv[4]);
        sqlite3_close(db);
        return res;
    } else if (strcmp(cmd, "query") == 0) {
        if (argc < 6) {
            print_json_error("Usage: samosa-chutni query <db_path> <root_id> <path_prefix> <search_query>", NULL);
            return EX_USAGE;
        }
        sqlite3* db;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) return EX_ERROR;
        int res = query_chutni(db, argv[3], argv[4], argv[5]);
        sqlite3_close(db);
        return res;
    } else if (strcmp(cmd, "forget") == 0) {
        if (argc < 4) {
            print_json_error("Usage: samosa-chutni forget <db_path> <root_id>", NULL);
            return EX_USAGE;
        }
        sqlite3* db;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) return EX_ERROR;
        int res = forget_root(db, argv[3]);
        sqlite3_close(db);
        return res;
    } else {
        print_json_error("Unknown command", cmd);
        return EX_USAGE;
    }
}
