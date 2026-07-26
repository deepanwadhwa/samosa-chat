import re

with open('src/samosa_chutni.c', 'r') as f:
    content = f.read()

# 1. Add json.h and tok.h includes
includes = """#include <unistd.h>
#include "sqlite/sqlite3.h"
#include "json.h"
#include "tok.h"
"""
content = content.replace('#include <unistd.h>\n#include "sqlite/sqlite3.h"', includes)

# 2. Add tokenizer loading
tok_init = """int init_db(const char* db_path) {"""
tok_load = """
static hmap T;
static int tok_initialized = 0;

void ensure_tokenizer() {
    if (!tok_initialized) {
        // Mock tokenization for MVP if tokenizer.json is missing
        // In a real implementation, we'd call tok_load(&T, "tokenizer.json");
        tok_initialized = 1;
    }
}

int init_db(const char* db_path) {"""
content = content.replace(tok_init, tok_load)

# 3. Add extract function
extract_func = """
void process_file(sqlite3* db, const char* root_id, const char* rel_path, const char* full_path) {
    char cmd[PATH_MAX + 100];
    snprintf(cmd, sizeof(cmd), "./build/samosa-extract --json \\"%s\\"", full_path);
    FILE* fp = popen(cmd, "r");
    if (!fp) return;
    
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\\0';
    pclose(fp);
    
    // Very simple extraction: just look for "text":"..."
    char* text_start = strstr(buf, "\\"text\\":\\"");
    if (text_start) {
        text_start += 8;
        char* text_end = strchr(text_start, '\\"');
        if (text_end) {
            *text_end = '\\0';
            
            // Insert chunk
            sqlite3_stmt* stmt;
            const char* sql = "INSERT INTO chunks_fts(content, file_id, chunk_index) VALUES (?, 1, 0)";
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, text_start, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            // Mark file as indexed
            const char* upd = "UPDATE files SET status='indexed', chunk_count=1 WHERE root_id=? AND rel_path=?";
            sqlite3_prepare_v2(db, upd, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, rel_path, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

void walk_directory(sqlite3_stmt* stmt, const char* root_id, const char* base_path, const char* rel_path) {
"""
content = content.replace('void walk_directory(sqlite3_stmt* stmt, const char* root_id, const char* base_path, const char* rel_path) {', extract_func)

# 4. In walk_directory, call process_file instead of just sqlite3_step if it's a regular file
walk_replace = """            if (skip) sqlite3_bind_text(stmt, 6, skip, -1, SQLITE_STATIC);
            else sqlite3_bind_null(stmt, 6);

            sqlite3_step(stmt);
            sqlite3_reset(stmt);
            
            if (!skip) {
                process_file(sqlite3_db_handle(stmt), root_id, next_rel_path, full_path);
            }"""
content = content.replace("""            if (skip) sqlite3_bind_text(stmt, 6, skip, -1, SQLITE_STATIC);
            else sqlite3_bind_null(stmt, 6);

            sqlite3_step(stmt);
            sqlite3_reset(stmt);""", walk_replace)

with open('src/samosa_chutni.c', 'w') as f:
    f.write(content)
