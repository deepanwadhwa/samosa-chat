import re

with open('src/samosa_chutni.c', 'r') as f:
    content = f.read()

# Add standard library headers for directory scanning
headers = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sqlite/sqlite3.h"
"""
content = re.sub(r'#include <stdio.h>.*?"sqlite/sqlite3.h"', headers, content, flags=re.DOTALL)

# Add scanning function
scan_logic = """
void print_json_error(const char* msg, const char* detail) {
    printf("{\\"status\\": \\"error\\", \\"message\\": \\"%s\\", \\"detail\\": \\"%s\\"}\\n", msg, detail ? detail : "");
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

    // Upsert root
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO roots (root_id, path, volume_identity, root_file_identity) VALUES (?, ?, ?, ?) "
                      "ON CONFLICT(root_id) DO UPDATE SET path=excluded.path, volume_identity=excluded.volume_identity, root_file_identity=excluded.root_file_identity;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, base_path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, dev_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, ino_str, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    // Very basic scan (MVP level)
    // This is just a stub that mocks scanning for now, we'll build recursive in the next step
    
    printf("{\\"status\\": \\"ok\\", \\"action\\": \\"scan\\", \\"root_id\\": \\"%s\\"}\\n", root_id);
    return EX_OK;
}
"""
content = content.replace('void print_json_error(const char* msg, const char* detail) {', scan_logic.split('void print_json_error')[0] + 'void print_json_error(const char* msg, const char* detail) {')
content = content.replace('int init_db(const char* db_path) {', scan_logic.split('}')[1] + '\n\nint init_db(const char* db_path) {')

# Add to main
main_logic = """
    if (strcmp(cmd, "init") == 0) {
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
    } else {
"""
content = content.replace("""    if (strcmp(cmd, "init") == 0) {
        return init_db(db_path);
    } else {""", main_logic)

with open('src/samosa_chutni.c', 'w') as f:
    f.write(content)
