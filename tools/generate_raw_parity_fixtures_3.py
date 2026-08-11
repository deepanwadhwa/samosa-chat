import mlx.core as mx
import mlx_lm
import json

prompts = [
    "Hello world!",
    "Summarize the plot of Romeo and Juliet in one sentence.",
    "Explain quantum computing simply."
]

def generate():
    print("Loading Python model...")
    model, tokenizer = mlx_lm.load("models/maple")

    refs = []
    for prompt in prompts:
        messages = [{"role": "user", "content": prompt}]
        formatted = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
        tokens = tokenizer.encode(formatted)
        x = mx.array([tokens], dtype=mx.int32)

        logits = model(x)
        last_logits = logits[0, -1, :]

        # Get top 20
        top20_idx = mx.argpartition(-last_logits, 20)[:20]
        # sort them
        sorted_top20 = top20_idx[mx.argsort(-last_logits[top20_idx])].tolist()

        refs.append({
            "prompt": prompt,
            "logits_shape": logits.shape,
            "input_tokens": tokens,
            "argmax_id": mx.argmax(last_logits).item(),
            "top20_ids": sorted_top20,
            "logits": last_logits.tolist() # This is large (151936), so maybe we don't save full logits to json?
            # Actually, to compute max abs error in C++, we need the full logits tensor.
        })
        print(f"Generated for prompt: {prompt[:20]}...")

    with open("tests/fixtures/maple/raw_parity_refs_3.json", "w") as f:
        json.dump(refs, f)

if __name__ == "__main__":
    generate()
