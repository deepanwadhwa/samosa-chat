import mlx.core as mx
import mlx_lm
import json
import os
import sys

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
    if os.path.exists("tests/fixtures/maple/greedy_parity_refs.json"):
        print("Fixtures already exist, skipping.")
        return

    print("Loading Python model to generate golden fixtures for 10 prompts x 32 tokens...")
    model, tokenizer = mlx_lm.load("models/maple")

    refs = []

    for i, prompt in enumerate(prompts):
        tokens = tokenizer.encode(prompt)
        x = mx.array([tokens], dtype=mx.int32)
        cache = mlx_lm.models.cache.make_prompt_cache(model)

        # Prefill
        logits = model(x, cache)
        last_logits = logits[:, -1, :]
        next_tok = mx.argmax(last_logits, axis=-1).item()

        gen_tokens = [next_tok]

        # Decode loop 31 times
        for _ in range(31):
            tok_arr = mx.array([[gen_tokens[-1]]], dtype=mx.int32)
            l = model(tok_arr, cache)
            next_t = mx.argmax(l, axis=-1).item()
            gen_tokens.append(next_t)

        refs.append({
            "prompt": prompt,
            "generated_tokens": gen_tokens
        })
        print(f"Generated {i+1}/10")

    with open("tests/fixtures/maple/greedy_parity_refs.json", "w") as f:
        json.dump(refs, f, indent=2)

if __name__ == "__main__":
    generate()
