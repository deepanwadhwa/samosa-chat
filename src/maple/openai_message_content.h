#ifndef SAMOSA_MAPLE_OPENAI_MESSAGE_CONTENT_H
#define SAMOSA_MAPLE_OPENAI_MESSAGE_CONTENT_H

#include "../json.h"

#include <string>

namespace samosa::maple {

// OpenAI chat messages may carry content either as a string or as an ordered
// array of typed parts.  Maple is text-only, so preserve every text part and
// ignore unsupported media parts without discarding the entire message.
inline bool openai_message_text(jval* content, std::string& text) {
    text.clear();
    if (!content) return false;
    if (content->t == J_STR) {
        text = content->str;
        return true;
    }
    if (content->t != J_ARR) return false;

    bool found_text = false;
    for (int i = 0; i < content->len; ++i) {
        jval* part = content->kids[i];
        if (!part || part->t != J_OBJ) continue;
        jval* type = json_get(part, "type");
        jval* value = json_get(part, "text");
        if (!type || type->t != J_STR || std::string(type->str) != "text" ||
            !value || value->t != J_STR) {
            continue;
        }
        if (found_text) text.push_back('\n');
        text.append(value->str);
        found_text = true;
    }
    return found_text;
}

}  // namespace samosa::maple

#endif
