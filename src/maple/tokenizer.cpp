#include "tokenizer.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>

#include "../tok.h"

namespace samosa {
namespace maple {

MapleTokenizer::MapleTokenizer(const std::string& model_dir) {
    std::string path = model_dir + "/tokenizer.json";

    Tok* t = new Tok();
    tok_load(t, path.c_str());
    tok_state_ = t;

    // We assume ChatML or Qwen special tokens. For Maple, the EOS is likely <|im_end|> or <|endoftext|>.
    // Let's resolve the actual IDs from the loaded tokenizer.
    eos_token_ = tok_id_of(t, "<|im_end|>");
    if (eos_token_ < 0) {
        eos_token_ = tok_id_of(t, "<|endoftext|>");
    }
    pad_token_ = eos_token_;
}

MapleTokenizer::~MapleTokenizer() {
    if (tok_state_) {
        Tok* t = static_cast<Tok*>(tok_state_);
        tok_free(t);
        delete t;
        tok_state_ = nullptr;
    }
}

std::vector<int> MapleTokenizer::encode(const std::string& text) const {
    Tok* t = static_cast<Tok*>(tok_state_);
    int max_tokens = text.size() + 128; // safe upper bound
    std::vector<int> out(max_tokens);
    int n = tok_encode(t, text.c_str(), text.size(), out.data(), max_tokens);
    out.resize(n);
    return out;
}

std::string MapleTokenizer::decode(const std::vector<int>& tokens) const {
    Tok* t = static_cast<Tok*>(tok_state_);
    int max_len = std::max(1, static_cast<int>(tokens.size() * 16 + 1));
    std::string out(max_len, '\0');
    int len = tok_decode(t, tokens.data(), tokens.size(), &out[0], max_len);
    out.resize(len);
    return out;
}

std::string MapleTokenizer::apply_chat_template(const std::vector<Message>& messages, bool add_generation_prompt,
                                                bool enable_thinking) const {
    std::string prompt;
    for (const auto& msg : messages) {
        prompt += "<|im_start|>" + msg.role + "\n" + msg.content + "<|im_end|>\n";
    }
    if (add_generation_prompt) {
        prompt += "<|im_start|>assistant\n";
        if (enable_thinking) prompt += "<think>\n";
    }
    return prompt;
}

} // namespace maple
} // namespace samosa
