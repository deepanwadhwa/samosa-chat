import re

with open('src/samosa_gateway.c', 'r') as f:
    content = f.read()

aggregate_old = """    } else if (!strcmp(req->path, "/v1/chutni/scope/summary")) {"""
aggregate_new = """    } else if (!strcmp(req->path, "/v1/chutni/scope/aggregate")) {
        ChooserRoot roots[MAX_CHOOSER_ROOTS];
        size_t num_roots = list_chooser_roots(g, roots, MAX_CHOOSER_ROOTS);
        
        char buf[65536] = {0};
        size_t offset = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset, "{\\"status\\":\\"ok\\", \\"roots\\": [");
        
        for (size_t i = 0; i < num_roots; i++) {
            char cmd_scan[PATH_MAX + 256];
            snprintf(cmd_scan, sizeof(cmd_scan), "./build/samosa-chutni scan /tmp/chutni.db %s %s", roots[i].id, roots[i].path);
            
            FILE *fp = popen(cmd_scan, "r");
            if (fp) {
                if (i > 0) offset += snprintf(buf + offset, sizeof(buf) - offset, ", ");
                size_t n = fread(buf + offset, 1, sizeof(buf) - offset - 1, fp);
                offset += n;
                pclose(fp);
            }
        }
        offset += snprintf(buf + offset, sizeof(buf) - offset, "]}");
        buf[offset] = '\\0';
        
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\\r\\nContent-Type: application/json\\r\\nContent-Length: %zu\\r\\n\\r\\n", offset);
        write(fd, header, strlen(header));
        write(fd, buf, offset);
        return 1;
    } else if (!strcmp(req->path, "/v1/chutni/scope/summary")) {"""

content = content.replace(aggregate_old, aggregate_new)

with open('src/samosa_gateway.c', 'w') as f:
    f.write(content)
