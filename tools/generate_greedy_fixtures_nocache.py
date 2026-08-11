import mlx.core as mx
import mlx_lm
import json

prompts = [
    "Hello world!",
    "What is the capital of France?",
    "Write a short poem about a cat.",
    "Summarize the plot of Romeo and Juliet in one sentence.",
    "Translate 'Good morning' to Spanish.",
    "Explain quantum computing simply.",
    "How do you make a chocolate cake?",
    "Who won the World Cup in 2018?",
    "What are the primary colors?",
    "List three benefits of regular exercise."
]

def generate():
    print("Loading Python model...")
    model, tokenizer = mlx_lm.load("models/maple")

    refs = []
    for i, prompt in enumerate(prompts):
        messages = [{"role": "user", "content": prompt}]
        formatted = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
        tokens = tokenizer.encode(formatted)
        x = mx.array([tokens], dtype=mx.int32)

        gen_tokens = []
        for step in range(32):
            logits = model(x)
            next_t = mx.argmax(logits[:, -1, :], axis=-1).item()
            gen_tokens.append(next_t)
            x = mx.concatenate([x, mx.array([[next_t]], dtype=mx.int32)], axis=1)
            mx.eval(x)
            mx.metal.clear_cache()

        refs.append({
            "prompt": prompt,
            "generated_tokens": gen_tokens
        })
        print(f"Generated {i+1}/10")

    with open("tests/fixtures/maple/greedy_parity_refs.json", "w") as f:
        json.dump(refs, f, indent=2)

if __name__ == "__main__":
    generate()
