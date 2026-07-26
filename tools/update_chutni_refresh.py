import re

with open('src/samosa_chutni.c', 'r') as f:
    content = f.read()

# 1. We need to fetch file_id to pass to process_file and chunks_fts
# Actually, process_file uses file_id=1 statically! Let's fix that too.
# Change process_file signature
process_file_sig = "void process_file(sqlite3* db, const char* root_id, const char* rel_path, const char* full_path) {"
process_file_new = """
void process_file(sqlite3* db, const char* root_id, const char* rel_path, const char* full_path, sqlite3_int64 file_id) {
    char cmd[PATH_MAX + 100];
    snprintf(cmd, sizeof(cmd), "./build/samosa-extract --json \\"%s\\"", full_path);
    FILE* fp = popen(cmd, "r");
    if (!fp) return;
    
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\\0';
    pclose(fp);
    
    char* text_start = strstr(buf, "\\"text\\":\\"");
    if (text_start) {
        text_start += 8;
        char* text_end = strchr(text_start, '\\"');
        if (text_end) {
            *text_end = '\\0';
            
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
"""
content = content.replace(process_file_sig, process_file_new)
content = content.replace('INSERT INTO chunks_fts(content, file_id, chunk_index) VALUES (?, 1, 0)', 'INSERT INTO chunks_fts(content, file_id, chunk_index) VALUES (?, ?, 0)')
content = content.replace('            // Mark file as indexed\n            const char* upd = "UPDATE files SET status=\'indexed\', chunk_count=1 WHERE root_id=? AND rel_path=?";\n            sqlite3_prepare_v2(db, upd, -1, &stmt, NULL);\n            sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_STATIC);\n            sqlite3_bind_text(stmt, 2, rel_path, -1, SQLITE_STATIC);\n            sqlite3_step(stmt);\n            sqlite3_finalize(stmt);\n', '')

# 2. Update walk_directory to do the incremental logic
walk_dir_old = """void walk_directory(sqlite3_stmt* stmt, const char* root_id, const char* base_path, const char* rel_path) {"""
walk_dir_new = """void walk_directory(sqlite3* db, const char* root_id, const char* base_path, const char* rel_path) {
    char current_dir[PATH_MAX];
    if (rel_path[0] == '\\0') snprintf(current_dir, sizeof(current_dir), "%s", base_path);
    else snprintf(current_dir, sizeof(current_dir), "%s/%s", base_path, rel_path);

    DIR* dir = opendir(current_dir);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // Skip hidden, ., ..

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, entry->d_name);

        char next_rel_path[PATH_MAX];
        if (rel_path[0] == '\\0') snprintf(next_rel_path, sizeof(next_rel_path), "%s", entry->d_name);
        else snprintf(next_rel_path, sizeof(next_rel_path), "%s/%s", rel_path, entry->d_name);

        struct stat st;
        if (lstat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            walk_directory(db, root_id, base_path, next_rel_path);
        } else if (S_ISREG(st.st_mode)) {
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
"""

# Replace walk_directory implementation
# We'll use regex to replace everything from `void walk_directory` to `int scan_directory`
content = re.sub(r'void walk_directory\(.*?\)\s*{.*?}\s*int scan_directory', walk_dir_new + '\n\nint scan_directory', content, flags=re.DOTALL)

# 3. Update scan_directory to use the new walk_directory and set unvisited
scan_dir_old = """    sqlite3_stmt* file_stmt;
    const char* file_sql = "INSERT INTO files (root_id, rel_path, size, mtime, status, skip_reason) VALUES (?, ?, ?, ?, ?, ?) "
                           "ON CONFLICT(root_id, rel_path) DO UPDATE SET size=excluded.size, mtime=excluded.mtime, status=excluded.status, skip_reason=excluded.skip_reason;";
    sqlite3_prepare_v2(db, file_sql, -1, &file_stmt, NULL);

    walk_directory(file_stmt, root_id, base_path, "");

    sqlite3_finalize(file_stmt);"""
scan_dir_new = """    // Mark all existing files as unvisited
    sqlite3_stmt* unv_stmt;
    sqlite3_prepare_v2(db, "UPDATE files SET status='unvisited' WHERE root_id=?", -1, &unv_stmt, NULL);
    sqlite3_bind_text(unv_stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_step(unv_stmt);
    sqlite3_finalize(unv_stmt);

    walk_directory(db, root_id, base_path, "");
    
    // Delete chunks for files that are still unvisited (i.e. deleted from disk)
    sqlite3_exec(db, "DELETE FROM chunks_fts WHERE file_id IN (SELECT id FROM files WHERE status='unvisited')", NULL, NULL, NULL);
    // Delete files that are unvisited
    sqlite3_exec(db, "DELETE FROM files WHERE status='unvisited'", NULL, NULL, NULL);"""
content = content.replace(scan_dir_old, scan_dir_new)

with open('src/samosa_chutni.c', 'w') as f:
    f.write(content)
