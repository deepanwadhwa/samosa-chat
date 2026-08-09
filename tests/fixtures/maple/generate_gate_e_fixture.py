import mlx.core as mx
import mlx_lm

def generate_fixture():
    print("Loading model...")
    model, _ = mlx_lm.load('models/maple')
    x = mx.array([[785, 3974, 13876, 38835, 34208, 916, 279, 15678, 5562]], dtype=mx.int32)
    
    cache = mlx_lm.models.cache.make_prompt_cache(model)
    logits_cached = []
    for i in range(x.shape[1]):
        token = x[:, i:i+1]
        l = model(token, cache)
        logits_cached.append(l)

    logits_cached = mx.concatenate(logits_cached, axis=1)
    
    mx.save_safetensors('tests/fixtures/maple/gate_e_fixture.safetensors', {
        'x': x,
        'expected_logits_cached': logits_cached
    })
    print("Saved gate_e_fixture.safetensors")

if __name__ == '__main__':
    generate_fixture()
