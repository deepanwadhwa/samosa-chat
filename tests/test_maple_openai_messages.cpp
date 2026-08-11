#include "maple/openai_message_content.h"

#include <cassert>
#include <iostream>
#include <string>

static std::string extract(const char* json, bool* ok = nullptr) {
    char* arena = nullptr;
    jval* root = json_parse(json, &arena);
    std::string text;
    const bool parsed = samosa::maple::openai_message_text(json_get(root, "content"), text);
    if (ok) *ok = parsed;
    json_free(root);
    free(arena);
    return text;
}

int main() {
    assert(extract(R"({"content":"plain question"})") == "plain question");
    assert(extract(R"({"content":[{"type":"text","text":"current question"}]})") ==
           "current question");
    assert(extract(R"({"content":[{"type":"text","text":"question"},{"type":"image_url","image_url":{"url":"x"}},{"type":"text","text":"web evidence"}]})") ==
           "question\nweb evidence");

    bool ok = true;
    assert(extract(R"({"content":[{"type":"image_url","image_url":{"url":"x"}}]})", &ok).empty());
    assert(!ok);
    assert(extract(R"({"content":42})", &ok).empty());
    assert(!ok);

    std::cout << "Maple OpenAI message parsing: PASS\n";
    return 0;
}
