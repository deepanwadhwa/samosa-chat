import mlx.core as mx
import mlx_lm
import json

prompts = [
    "Hello world!",
    "What is the capital of France?",
    "Write a short poem about a cat."
]

def run():
    model, tokenizer = mlx_lm.load('models/maple')

    for i, prompt in enumerate(prompts):
        tokens = tokenizer.encode(prompt)
        x = mx.array([tokens], dtype=mx.int32)

        logits = model(x)
        last_logits = logits[:, -1, :]

        argmax_id = mx.argmax(last_logits, axis=-1).item()

        # Top 20
        sorted_indices = mx.argsort(last_logits, axis=-1)[0]
        top_20 = sorted_indices[-20:].tolist()[::-1] # highest first

        mx.save_safetensors(f'tests/fixtures/maple/raw_parity_{i}.safetensors', {
            'x': x,
            'expected_logits': last_logits
        })

        with open(f'tests/fixtures/maple/raw_parity_{i}.json', 'w') as f:
            json.dump({
                "prompt": prompt,
                "tokens": tokens,
                "shape": last_logits.shape,
                "argmax": argmax_id,
                "top_20": top_20
            }, f, indent=2)

if __name__ == '__main__':
    run()
