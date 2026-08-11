import mlx.core as mx
import mlx_lm
import numpy as np

def generate_fixture():
    print("Loading model...")
    model, _ = mlx_lm.load('models/maple')

    inputs = mx.array([[785, 3974, 13876, 38835, 34208, 916, 279, 15678, 5562]], dtype=mx.int32)
    cache = mlx_lm.models.cache.make_prompt_cache(model)

    generated_tokens = []
    y = inputs
    print("Starting generation...")
    for step in range(5):
        logits = model(y, cache)
        logits = logits[:, -1, :]
        token = mx.argmax(logits, axis=-1)
        generated_tokens.append(token.item())
        y = token[:, None]
        print(f"Step {step}: token {token.item()}")

    generated_tokens_arr = mx.array(generated_tokens, dtype=mx.int32)

    mx.save_safetensors('tests/fixtures/maple/gate_d_fixture.safetensors', {
        'x': inputs,
        'expected_tokens': generated_tokens_arr
    })
    print("Saved gate_d_fixture.safetensors")

if __name__ == '__main__':
    generate_fixture()
