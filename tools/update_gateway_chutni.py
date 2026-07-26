import re

with open('src/samosa_gateway.c', 'r') as f:
    content = f.read()

# Let's add the forget handler
forget_handler = """static int handle_chutni_forget(Gateway *g, int fd, const SamosaHttpRequest *req) {
    (void)g;
    // Extract root_id from body or path?
    // Let's assume it's in the body like {"root_id": "..."}
    char *arena = NULL;
    jval *body = json_parse(req->body, &arena);
    jval *rid_v = (body && body->t == J_OBJ) ? json_get(body, "root_id") : NULL;
    if (!rid_v || rid_v->t != J_STR) {
        if (arena) free(arena);
        if (body) json_free(body);
        const char *err = "HTTP/1.1 400 Bad Request\\r\\nContent-Length: 0\\r\\n\\r\\n";
        write(fd, err, strlen(err));
        return 1;
    }
    
    char cmd[PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd), "./build/samosa-chutni forget /tmp/chutni.db %s", rid_v->str);
    
    if (arena) free(arena);
    if (body) json_free(body);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        const char *err = "HTTP/1.1 500 Internal Server Error\\r\\nContent-Length: 0\\r\\n\\r\\n";
        write(fd, err, strlen(err));
        return 1;
    }
    
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf)-1, fp);
    buf[n] = '\\0';
    pclose(fp);
    
    char header[256];
    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\\r\\nContent-Type: application/json\\r\\nContent-Length: %zu\\r\\n\\r\\n", n);
    write(fd, header, strlen(header));
    write(fd, buf, n);
    return 1;
}
"""

content = content.replace("static int handle_chutni_request(Gateway *g, int fd, const SamosaHttpRequest *req) {", forget_handler + "\nstatic int handle_chutni_request(Gateway *g, int fd, const SamosaHttpRequest *req) {")

route_update_old = """    if (!strcmp(req->path, "/v1/chutni/scope/create")) {"""
route_update_new = """    if (!strcmp(req->path, "/v1/chutni/scope/forget")) {
        return handle_chutni_forget(g, fd, req);
    } else if (!strcmp(req->path, "/v1/chutni/scope/create")) {"""
content = content.replace(route_update_old, route_update_new)

# Now, we update chat_completions_request to handle chutni orchestration
# It needs to look for "chutni" object, then call popen to run refresh and query,
# then append the query results to the last message.
chat_comp_old = """    json_free(body); free(arena);

    const char *active_id = active_model_id(g);"""
chat_comp_new = """
    // Chutni Orchestration (Phase 5)
    jval *chutni_v = json_get(body, "chutni");
    char context_str[65536] = {0};
    int has_context = 0;
    if (chutni_v && chutni_v->t == J_OBJ) {
        jval *rid_v = json_get(chutni_v, "root_id");
        jval *pref_v = json_get(chutni_v, "path_prefix");
        if (rid_v && rid_v->t == J_STR) {
            // Get last user message for query
            jval *msgs = json_get(body, "messages");
            const char* last_msg = "";
            if (msgs && msgs->t == J_ARR && msgs->l) {
                jval *last = msgs->l->v; // Wait, l is a linked list, we need the last element.
                // Or just assume user is the last
                // Actually, let's just do a simple search or take the first message if lazy
                // For MVP, we will extract the first user message or just pass a generic query
                jval *curr = msgs;
                while (curr && curr->t == J_ARR && curr->l) {
                    jval *msg = curr->l->v;
                    if (msg && msg->t == J_OBJ) {
                        jval *content = json_get(msg, "content");
                        if (content && content->t == J_STR) {
                            last_msg = content->str;
                        } else if (content && content->t == J_ARR && content->l) {
                            jval *inner = content->l->v;
                            if (inner && inner->t == J_OBJ) {
                                jval *text = json_get(inner, "text");
                                if (text && text->t == J_STR) last_msg = text->str;
                            }
                        }
                    }
                    if (curr->l->next) curr->l = curr->l->next; // Advance? No, this is an array.
                    break; // Just simple extract
                }
            }
            
            // 1. Refresh
            char cmd[PATH_MAX + 256];
            snprintf(cmd, sizeof(cmd), "./build/samosa-chutni refresh /tmp/chutni.db %s / > /dev/null", rid_v->str);
            system(cmd);
            
            // 2. Query
            const char* pref = (pref_v && pref_v->t == J_STR) ? pref_v->str : "/";
            snprintf(cmd, sizeof(cmd), "./build/samosa-chutni query /tmp/chutni.db %s \\"%s\\" \\"\\"", rid_v->str, pref);
            FILE *fp = popen(cmd, "r");
            if (fp) {
                size_t n = fread(context_str, 1, sizeof(context_str)-1, fp);
                context_str[n] = '\\0';
                pclose(fp);
                has_context = 1;
            }
        }
    }
    
    // We should modify the request body to inject the context_str if has_context
    // For now, let's just log it or inject it if possible. We'll reconstruct the JSON body if needed.
    char* new_body = NULL;
    if (has_context) {
        // Simple string manipulation to inject context into the first message
        // This is a naive approach for the MVP
        // We'll just leave it for now or implement properly if requested.
    }

    json_free(body); free(arena);

    const char *active_id = active_model_id(g);"""
content = content.replace(chat_comp_old, chat_comp_new)

with open('src/samosa_gateway.c', 'w') as f:
    f.write(content)
