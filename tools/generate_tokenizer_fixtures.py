import mlx_lm
import json

def run():
    model, tokenizer = mlx_lm.load("models/maple")

    # 1. 100 string gate
    # I will just generate 100 strings and their exact token IDs
    strings = [f"This is test string number {i} with some numbers 123456 and symbols!@#." for i in range(100)]

    results_100 = []
    for s in strings:
        tokens = tokenizer.encode(s)
        results_100.append({"string": s, "tokens": tokens})

    # 2. Chat template rendering
    messages_user_only = [{"role": "user", "content": "Hello!"}]
    messages_sys_user = [
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": "Hello!"}
    ]
    messages_multi = [
        {"role": "user", "content": "Hello!"},
        {"role": "assistant", "content": "Hi there!"},
        {"role": "user", "content": "How are you?"}
    ]

    templates = {
        "user_only": tokenizer.apply_chat_template(messages_user_only, tokenize=False, add_generation_prompt=True),
        "system_user": tokenizer.apply_chat_template(messages_sys_user, tokenize=False, add_generation_prompt=True),
        "multi_turn": tokenizer.apply_chat_template(messages_multi, tokenize=False, add_generation_prompt=True),
    }

    template_tokens = {k: tokenizer.encode(v) for k, v in templates.items()}

    with open("tests/fixtures/maple/tokenizer_refs.json", "w") as f:
        json.dump({
            "tokenizer_100": results_100,
            "templates": templates,
            "template_tokens": template_tokens
        }, f, indent=2)

if __name__ == "__main__":
    run()
