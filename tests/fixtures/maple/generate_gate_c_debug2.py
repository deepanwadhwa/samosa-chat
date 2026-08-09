import mlx.core as mx
import mlx_lm

model_path = "models/maple"
model, tokenizer = mlx_lm.load(model_path)

prompt = "The quick brown fox jumps over the lazy dog"
tokens = tokenizer.encode(prompt)
x = mx.array([tokens])

h = model.model.word_embeddings(x)

cache = [None] * len(model.model.layers)
mask = mlx_lm.models.base.create_attention_mask(h, cache[0])

for i, layer in enumerate(model.model.layers):
    h = layer(h, mask=mask, cache=cache[i])
    if i == 0 or i == 23:
        mx.eval(h)
        print(f"DEBUG (Python): Layer {i} out mean absolute: {mx.mean(mx.abs(h)).item()}")
