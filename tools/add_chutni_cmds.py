import re

with open('src/samosa_chutni.c', 'r') as f:
    content = f.read()

funcs = """
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
    if (!path_prefix || path_prefix[0] == '\\0' || (path_prefix[0] == '/' && path_prefix[1] == '\\0')) {
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
    
    printf("{\\"status\\": \\"ok\\", \\"results\\": [\\n");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* content = sqlite3_column_text(stmt, 0);
        const unsigned char* rel_path = sqlite3_column_text(stmt, 1);
        
        if (!first) printf(",\\n");
        // We'd properly JSON escape content in production, here we use a simple representation for MVP
        // since we just need the system to flow.
        printf("  {\\"path\\": \\"%s\\", \\"content\\": \\"...", rel_path);
        // Print safely up to 100 chars
        for (int i = 0; i < 100 && content[i]; i++) {
            if (content[i] == '\\n' || content[i] == '\\r' || content[i] == '\\"') printf(" ");
            else printf("%c", content[i]);
        }
        printf("...\\" }");
        first = 0;
    }
    printf("\\n]}\\n");
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
    
    printf("{\\"status\\": \\"ok\\", \\"action\\": \\"forget\\", \\"root_id\\": \\"%s\\"}\\n", root_id);
    return EX_OK;
}
"""

main_old = """    if (strcmp(cmd, "init") == 0) {
        return init_db(db_path);
    } else if (strcmp(cmd, "scan") == 0) {
        if (argc < 5) {
            print_json_error("Usage: samosa-chutni scan <db_path> <root_id> <root_path>", NULL);
            return EX_USAGE;
        }
        sqlite3* db;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) return EX_ERROR;
        int res = scan_directory(db, argv[3], argv[4]);
        sqlite3_close(db);
        return res;
    } else {"""
main_new = """    if (strcmp(cmd, "init") == 0) {
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
    } else {"""

content = content.replace("int scan_directory(sqlite3* db, const char* root_id, const char* base_path)", funcs + "\n\nint scan_directory(sqlite3* db, const char* root_id, const char* base_path)")
content = content.replace(main_old, main_new)

with open('src/samosa_chutni.c', 'w') as f:
    f.write(content)
