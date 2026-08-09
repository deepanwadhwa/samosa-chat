import mlx.core as mx
import mlx_lm

model_path = "models/maple"
model, tokenizer = mlx_lm.load(model_path)

prompt = "The quick brown fox jumps over the lazy dog"
tokens = tokenizer.encode(prompt)
x = mx.array([tokens])

h = model.model.word_embeddings(x)
mx.eval(h)
print(f"DEBUG (Python): Embeddings out mean absolute: {mx.mean(mx.abs(h)).item()}")
