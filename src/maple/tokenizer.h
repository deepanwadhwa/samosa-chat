#ifndef SAMOSA_MAPLE_TOKENIZER_H
#define SAMOSA_MAPLE_TOKENIZER_H

#include <string>
#include <vector>

namespace samosa {
namespace maple {

struct Message {
    std::string role;
    std::string content;
};

class MapleTokenizer {
public:
    MapleTokenizer(const std::string& model_dir);
    ~MapleTokenizer();

    std::vector<int> encode(const std::string& text) const;
    std::string decode(const std::vector<int>& tokens) const;
    
    // Applies the model's chat template to generate a prompt.
    // DeepGrove Maple uses Qwen-style ChatML.
    std::string apply_chat_template(const std::vector<Message>& messages, bool add_generation_prompt) const;
    
    int get_eos_token() const { return eos_token_; }
    int get_pad_token() const { return pad_token_; }

private:
    void* tok_state_; // Pointer to Tok struct
    int eos_token_ = -1;
    int pad_token_ = -1;
};

} // namespace maple
} // namespace samosa

#endif
