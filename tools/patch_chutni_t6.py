import re

with open('src/samosa_chutni.c', 'r') as f:
    content = f.read()

# 1. Add should_exclude_path
exclude_func = """
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
"""
content = content.replace("void walk_directory(sqlite3* db, const char* root_id, const char* base_path, const char* rel_path)", 
                          exclude_func + "\nvoid walk_directory(sqlite3* db, const char* root_id, const char* base_path, const char* rel_path, dev_t root_dev, int* files_scanned)")

# 2. Update walk_directory signature and logic
walk_old = """    char current_dir[PATH_MAX];
    if (rel_path[0] == '\\0') snprintf(current_dir, sizeof(current_dir), "%s", base_path);
    else snprintf(current_dir, sizeof(current_dir), "%s/%s", base_path, rel_path);

    DIR* dir = opendir(current_dir);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // Skip hidden, ., ..

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, entry->d_name);"""

walk_new = """    if (*files_scanned >= 50000) return; // Quota limit

    char current_dir[PATH_MAX];
    if (rel_path[0] == '\\0') snprintf(current_dir, sizeof(current_dir), "%s", base_path);
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
        
        if (should_exclude_path(full_path)) continue;"""
content = content.replace(walk_old, walk_new)

walk_rec_old = """        if (S_ISDIR(st.st_mode)) {
            walk_directory(db, root_id, base_path, next_rel_path);
        } else if (S_ISREG(st.st_mode)) {"""
walk_rec_new = """        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev == root_dev) {
                walk_directory(db, root_id, base_path, next_rel_path, root_dev, files_scanned);
            }
        } else if (S_ISREG(st.st_mode)) {
            (*files_scanned)++;"""
content = content.replace(walk_rec_old, walk_rec_new)

# 3. Update scan_directory
scan_old = """    snprintf(dev_str, sizeof(dev_str), "%llu", (unsigned long long)st.st_dev);
    snprintf(ino_str, sizeof(ino_str), "%llu", (unsigned long long)st.st_ino);

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
    sqlite3_finalize(root_stmt);"""

scan_new = """    snprintf(dev_str, sizeof(dev_str), "%llu", (unsigned long long)st.st_dev);
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
    sqlite3_finalize(root_stmt);"""
content = content.replace(scan_old, scan_new)

scan_walk_old = """    walk_directory(db, root_id, base_path, "");"""
scan_walk_new = """    int files_scanned = 0;
    walk_directory(db, root_id, base_path, "", st.st_dev, &files_scanned);"""
content = content.replace(scan_walk_old, scan_walk_new)

scan_finish_old = """    printf("{\\"status\\": \\"ok\\", \\"action\\": \\"scan\\", \\"root_id\\": \\"%s\\", \\"files_seen\\": %d, \\"files_skipped\\": %d}\\n", root_id, files_seen, files_skipped);
    return EX_OK;"""
scan_finish_new = """    if (files_scanned >= 50000) {
        printf("{\\"status\\": \\"quota_exceeded\\", \\"action\\": \\"scan\\", \\"root_id\\": \\"%s\\", \\"files_seen\\": %d, \\"files_skipped\\": %d}\\n", root_id, files_seen, files_skipped);
    } else {
        printf("{\\"status\\": \\"ok\\", \\"action\\": \\"scan\\", \\"root_id\\": \\"%s\\", \\"files_seen\\": %d, \\"files_skipped\\": %d}\\n", root_id, files_seen, files_skipped);
    }
    return EX_OK;"""
content = content.replace(scan_finish_old, scan_finish_new)

with open('src/samosa_chutni.c', 'w') as f:
    f.write(content)
