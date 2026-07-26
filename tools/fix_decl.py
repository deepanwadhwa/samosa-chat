with open('src/samosa_gateway.c', 'r') as f:
    content = f.read()

# Extract the block from #define MAX_CHOOSER_ROOTS 32 down to the end of list_chooser_roots
start_str = "#define MAX_CHOOSER_ROOTS 32"
end_str = "    return n;\n}\n"

start_idx = content.find(start_str)
end_idx = content.find(end_str, start_idx) + len(end_str)

block = content[start_idx:end_idx]

# Remove it from its current location
content = content[:start_idx] + content[end_idx:]

# Insert it before handle_chutni_forget (around line 4779)
insert_target = "static int handle_chutni_forget(Gateway *g, int fd, const SamosaHttpRequest *req) {"
insert_idx = content.find(insert_target)

content = content[:insert_idx] + block + "\n" + content[insert_idx:]

with open('src/samosa_gateway.c', 'w') as f:
    f.write(content)
